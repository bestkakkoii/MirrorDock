// SPDX-License-Identifier: MIT
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <netlistmgr.h>
#include <objbase.h>
#include <wrl/client.h>

#include "uxplay_version.h"

module uxplay_update_check;

import std;
import uxplay_status;
import uxplay_log;
import uxplay_settings;
import uxplay_http_client;
import uxplay_property_list;

namespace uxplay
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        // per-user settings (uxplay_settings)
        constexpr std::wstring_view setting_automatic = L"update_check_automatic"; // 0 off, 1 on (default)
        // seconds since 1970 (UTC) of the last check that got an answer
        constexpr std::wstring_view setting_checked_at = L"update_checked_at";
        // the version that check found (packed_version)
        constexpr std::wstring_view setting_newest = L"update_newest_version";

        constexpr auto first_check_delay = std::chrono::seconds{30};
        constexpr auto check_interval = std::chrono::hours{24};
        constexpr auto retry_interval = std::chrono::hours{1};
        // for each step of the request (name, connection, sending, every piece received)
        constexpr auto request_patience = std::chrono::seconds{15};
        constexpr std::size_t longest_answer = 1024 * 1024;
        constexpr unsigned int largest_version_part = 1023;

        constexpr std::string_view project_url = UXPLAY_PROJECT_URL;
        constexpr std::string_view tag_prefix = UXPLAY_RELEASE_TAG_PREFIX;
        constexpr std::string_view package_name = UXPLAY_RELEASE_PACKAGE_NAME;
        // of whatever release is the newest: GitHub redirects these
        constexpr char newest_page_url[] = UXPLAY_PROJECT_URL "/releases/latest";
        constexpr char newest_package_url[] = UXPLAY_PROJECT_URL "/releases/latest/download/" UXPLAY_RELEASE_PACKAGE_NAME;

        [[nodiscard]] std::uint32_t packed_version(release_version version)
        {
            return (version.major << 20) | (version.minor << 10) | version.patch;
        }

        [[nodiscard]] release_version unpacked_version(std::uint32_t packed)
        {
            return release_version{packed >> 20, (packed >> 10) & largest_version_part, packed & largest_version_part};
        }

        // An address below the project's own pages, written with nothing but
        // the characters such an address has: only that is ever handed to
        // the browser.
        [[nodiscard]] bool project_address(std::string_view url)
        {
            if (!url.starts_with(project_url) || url.size() <= project_url.size() || url[project_url.size()] != '/' ||
                url.size() > 512)
            {
                return false;
            }
            return std::ranges::all_of(url, [](char const character)
            {
                return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9') ||
                       std::string_view{"-._~:/%"}.contains(character);
            });
        }

        // text that came from the network, for a message: short, on one line
        [[nodiscard]] std::string limited(std::string_view text)
        {
            std::string result{text.substr(0, 160)};
            std::ranges::replace_if(
                result,
                [](char const character)
                {
                    return static_cast<unsigned char>(character) < 0x20;
                },
                ' ');
            return result;
        }

        [[nodiscard]] bool is_date(std::string_view text)
        {
            if (text.size() != 10)
            {
                return false;
            }
            for (std::size_t index = 0; index < text.size(); ++index)
            {
                bool const dash = index == 4 || index == 7;
                if (dash ? text[index] != '-' : (text[index] < '0' || text[index] > '9'))
                {
                    return false;
                }
            }
            return true;
        }

        // "2026-11-12T20:41:13Z" as the day it was at this PC then (east of
        // Greenwich that can be the 13th); empty when the text is no such time.
        [[nodiscard]] std::string local_day(std::string_view utc)
        {
            if (utc.size() < 19 || !is_date(utc.substr(0, 10)) || utc[10] != 'T' || utc[13] != ':' || utc[16] != ':')
            {
                return {};
            }
            bool valid = true;
            auto const number = [&utc, &valid](std::size_t offset, std::size_t digits)
            {
                unsigned int value = 0;
                for (std::size_t index = offset; index < offset + digits; ++index)
                {
                    valid = valid && utc[index] >= '0' && utc[index] <= '9';
                    value = value * 10 + static_cast<unsigned int>(utc[index] - '0');
                }
                return static_cast<WORD>(value);
            };
            SYSTEMTIME universal{};
            universal.wYear = number(0, 4);
            universal.wMonth = number(5, 2);
            universal.wDay = number(8, 2);
            universal.wHour = number(11, 2);
            universal.wMinute = number(14, 2);
            universal.wSecond = number(17, 2);
            SYSTEMTIME local{};
            if (!valid || SystemTimeToTzSpecificLocalTime(nullptr, &universal, &local) == FALSE)
            {
                return {};
            }
            return std::format("{:04}-{:02}-{:02}", local.wYear, local.wMonth, local.wDay);
        }

        // The request on a thread of its own, so that the end of the program
        // does not wait for a server that does not answer: the checker stops
        // waiting and leaves that thread to end alone.  It uses nothing but
        // this block, which it owns together with the checker.
        struct pending_request
        {
            std::mutex mutex;
            std::condition_variable_any finished;
            std::optional<outcome<published_release>> answer; // guarded by mutex
            std::atomic<bool> abandoned{false};
        };

        // nothing when the stop was requested first
        [[nodiscard]] std::optional<outcome<published_release>> ask(std::stop_token stop)
        {
            auto const shared = std::make_shared<pending_request>();
            try
            {
                std::thread{[shared]
                {
                    auto answer = newest_release([shared]
                    {
                        return shared->abandoned.load();
                    });
                    {
                        std::scoped_lock lock{shared->mutex};
                        shared->answer = std::move(answer);
                    }
                    shared->finished.notify_all();
                }}.detach();
            }
            catch (std::system_error const &cause)
            {
                return std::unexpected(make_failure("thread for the request", cause.code().value(), cause.what()));
            }
            std::unique_lock lock{shared->mutex};
            if (!shared->finished.wait(lock, stop, [&shared]
                {
                    return shared->answer.has_value();
                }))
            {
                shared->abandoned.store(true);
                return std::nullopt;
            }
            return std::move(shared->answer);
        }

        struct com_scope
        {
            HRESULT entered = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

            com_scope() = default;
            com_scope(com_scope const &) = delete;
            com_scope &operator=(com_scope const &) = delete;

            ~com_scope()
            {
                if (SUCCEEDED(entered))
                {
                    CoUninitialize();
                }
            }
        };
    }

    std::string release_version::text() const
    {
        return std::format("{}.{}.{}", major, minor, patch);
    }

    release_version running_version()
    {
        return release_version{UXPLAY_VERSION_MAJOR, UXPLAY_VERSION_MINOR, UXPLAY_VERSION_PATCH};
    }

    std::optional<release_version> release_version_from_tag(std::string_view tag)
    {
        if (!tag.starts_with(tag_prefix))
        {
            return std::nullopt;
        }
        tag.remove_prefix(tag_prefix.size());
        std::array<unsigned int, 3> parts{};
        std::size_t count = 0;
        for (;;)
        {
            if (count == parts.size())
            {
                return std::nullopt;
            }
            unsigned int value = 0;
            char const *const begin = tag.data();
            auto const [end, problem] = std::from_chars(begin, begin + tag.size(), value);
            if (problem != std::errc{} || end == begin || value > largest_version_part)
            {
                return std::nullopt;
            }
            parts[count++] = value;
            tag.remove_prefix(static_cast<std::size_t>(end - begin));
            if (tag.empty())
            {
                break;
            }
            if (tag.front() != '.' || tag.size() == 1)
            {
                return std::nullopt;
            }
            tag.remove_prefix(1);
        }
        if (count < 2)
        {
            return std::nullopt;
        }
        return release_version{parts[0], parts[1], parts[2]};
    }

    outcome<published_release> parse_release(std::string_view json)
    {
        auto document = property_list::parse_json(json);
        if (!document)
        {
            return std::unexpected(make_failure("release description", document.error().code,
                                                std::format("not readable ({})", document.error().describe())));
        }
        property_node const root = document->root();
        std::optional<std::string> const tag = root.item("tag_name").text();
        if (!tag)
        {
            return std::unexpected(make_failure("release description", 0, "it names no release (no \"tag_name\")"));
        }
        std::optional<release_version> const version = release_version_from_tag(*tag);
        if (!version)
        {
            return std::unexpected(
                make_failure("release description", 0,
                             std::format("the tag \"{}\" has not the form {}<major>.<minor>[.<patch>]", limited(*tag),
                                         tag_prefix)));
        }
        if (root.item("draft").flag().value_or(false) || root.item("prerelease").flag().value_or(false))
        {
            return std::unexpected(make_failure("release description", 0,
                                                std::format("{} is a draft or a pre-release", limited(*tag))));
        }
        published_release release;
        release.version = *version;
        if (std::optional<std::string> const published = root.item("published_at").text(); published)
        {
            release.date = local_day(*published);
        }
        release.page_url = newest_page_url;
        release.package_url = newest_package_url;
        if (std::optional<std::string> const page = root.item("html_url").text(); page && project_address(*page))
        {
            release.page_url = *page;
        }
        property_node const files = root.item("assets");
        for (std::size_t index = 0; index < files.size(); ++index)
        {
            property_node const file = files.element(index);
            if (file.item("name").text().value_or(std::string{}) != package_name)
            {
                continue;
            }
            if (std::optional<std::string> const address = file.item("browser_download_url").text();
                address && project_address(*address))
            {
                release.package_url = *address;
            }
            release.package_bytes = file.item("size").number().value_or(0);
            break;
        }
        return release;
    }

    outcome<published_release> newest_release(std::function<bool()> cancelled)
    {
        http_request request;
        request.url = UXPLAY_RELEASE_FEED_URL;
        // GitHub refuses a request without one; this is all it learns besides the address it is asked from
        request.user_agent = UXPLAY_PRODUCT_NAME "/" UXPLAY_VERSION_TEXT;
        request.headers = {"Accept: application/vnd.github+json"};
        request.longest_answer = longest_answer;
        request.patience = request_patience;
        request.cancelled = std::move(cancelled);
        auto answer = http_exchange(request);
        if (!answer)
        {
            return std::unexpected(answer.error());
        }
        std::string_view const body{reinterpret_cast<char const *>(answer->body.data()), answer->body.size()};
        if (answer->status != 200)
        {
            // GitHub says in "message" why it refuses (too many requests from this address, say)
            std::string reason;
            if (auto document = property_list::parse_json(body); document)
            {
                reason = limited(document->root().item("message").text().value_or(std::string{}));
            }
            return std::unexpected(make_failure("GitHub", answer->status,
                                                reason.empty() ? "it answered with this HTTP status" : reason));
        }
        return parse_release(body);
    }

    std::optional<bool> metered_connection()
    {
        ComPtr<INetworkCostManager> manager;
        if (FAILED(CoCreateInstance(__uuidof(NetworkListManager), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&manager))))
        {
            return std::nullopt;
        }
        DWORD cost = 0;
        if (FAILED(manager->GetCost(&cost, nullptr)))
        {
            return std::nullopt;
        }
        constexpr DWORD paid_by_volume = NLM_CONNECTION_COST_FIXED | NLM_CONNECTION_COST_VARIABLE |
                                         NLM_CONNECTION_COST_OVERDATALIMIT | NLM_CONNECTION_COST_ROAMING;
        return (cost & paid_by_volume) != 0;
    }

    struct update_checker::state
    {
        update_events events;
        mutable std::mutex mutex;
        std::condition_variable_any wake;
        update_state shown;         // guarded by mutex
        bool requested = false;     // guarded: the user asked for a check
        unsigned int changes = 0;   // guarded: counts what the waiting thread has to look at again
        std::chrono::system_clock::time_point due; // guarded: the next automatic check
        bool failure_logged = false; // checker thread: an automatic check failed in this run
        std::jthread worker;        // last member: stopped and joined before the others go

        void announce(bool by_request) const;
        void run(std::stop_token stop);
    };

    void update_checker::state::announce(bool by_request) const
    {
        update_state copy;
        {
            std::scoped_lock lock{mutex};
            copy = shown;
        }
        if (events.on_change)
        {
            events.on_change(copy, by_request);
        }
    }

    void update_checker::state::run(std::stop_token stop)
    {
        com_scope const com;
        for (;;)
        {
            bool by_request = false;
            {
                std::unique_lock lock{mutex};
                for (;;)
                {
                    if (stop.stop_requested())
                    {
                        return;
                    }
                    if (std::exchange(requested, false))
                    {
                        by_request = true;
                        break;
                    }
                    unsigned int const seen = changes;
                    auto const changed = [this, seen]
                    {
                        return requested || changes != seen;
                    };
                    if (!shown.automatic)
                    {
                        wake.wait(lock, stop, changed);
                    }
                    else if (std::chrono::system_clock::now() >= due)
                    {
                        break;
                    }
                    else
                    {
                        wake.wait_until(lock, stop, due, changed);
                    }
                }
            }
            if (!by_request)
            {
                bool const metered = metered_connection().value_or(false);
                if (metered || (events.automatic_allowed && !events.automatic_allowed()))
                {
                    log::debug("update check: not now ({}); again in an hour",
                               metered ? "Windows says the connection is metered"
                                       : "the PC's internet goes through the iPhone");
                    std::scoped_lock lock{mutex};
                    due = std::chrono::system_clock::now() + retry_interval;
                    continue;
                }
            }
            update_phase before = update_phase::unknown;
            {
                std::scoped_lock lock{mutex};
                before = shown.phase;
                shown.phase = update_phase::checking;
            }
            announce(by_request);
            std::optional<outcome<published_release>> const answer = ask(stop);
            if (!answer)
            {
                return; // the program ends
            }
            auto const now = std::chrono::system_clock::now();
            if (*answer)
            {
                published_release const &newest = **answer;
                {
                    std::scoped_lock lock{mutex};
                    shown.newest = newest;
                    shown.checked = now;
                    shown.problem.clear();
                    shown.phase = newest.version > running_version() ? update_phase::newer_available
                                                                       : update_phase::up_to_date;
                    due = now + check_interval;
                }
                auto const seconds =
                    std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
                settings::store_number(setting_checked_at, static_cast<std::uint32_t>(seconds));
                settings::store_number(setting_newest, packed_version(newest.version));
                log::info("update check: the newest release is {}{}{}; this is {}", newest.version.text(),
                          newest.date.empty() ? std::string{} : std::format(" ({})", newest.date),
                          newest.package_bytes == 0 ? std::string{", no package found among its files"}
                                                    : std::format(", package of {} bytes", newest.package_bytes),
                          running_version().text());
            }
            else
            {
                std::string const problem = answer->error().describe();
                {
                    std::scoped_lock lock{mutex};
                    shown.problem = problem;
                    // a check nobody asked for leaves what the user sees as it was
                    shown.phase = by_request ? update_phase::failed : before;
                    due = std::max(due, now + retry_interval);
                }
                if (by_request)
                {
                    log::warning("update check: {}", problem);
                }
                else if (!std::exchange(failure_logged, true))
                {
                    log::info("update check: no answer ({}); tried again every hour, without a further line here",
                              problem);
                }
            }
            announce(by_request);
        }
    }

    update_checker::update_checker(std::unique_ptr<state> created_state)
        : impl{std::move(created_state)}
    {
    }

    update_checker::~update_checker() = default;

    std::unique_ptr<update_checker> update_checker::create(update_events events)
    {
        auto created = std::make_unique<state>();
        created->events = std::move(events);
        created->shown.automatic = settings::stored_number(setting_automatic).value_or(1) != 0;
        auto const now = std::chrono::system_clock::now();
        created->due = now + first_check_delay;
        if (std::optional<std::uint32_t> const checked_at = settings::stored_number(setting_checked_at);
            checked_at && *checked_at != 0)
        {
            std::chrono::system_clock::time_point const checked = std::chrono::sys_seconds{std::chrono::seconds{*checked_at}};
            // (a time in the future: the clock was set back; then nothing is known)
            if (checked <= now)
            {
                created->shown.checked = checked;
                created->due = std::max(created->due, checked + check_interval);
                if (std::optional<std::uint32_t> const newest = settings::stored_number(setting_newest);
                    newest && *newest != 0)
                {
                    published_release release;
                    release.version = unpacked_version(*newest);
                    release.page_url = newest_page_url;
                    release.package_url = newest_package_url;
                    created->shown.phase = release.version > running_version() ? update_phase::newer_available
                                                                                 : update_phase::up_to_date;
                    created->shown.newest = std::move(release);
                }
            }
        }
        log::info("update check: automatic {}", created->shown.automatic ? "on (once a day)" : "off");
        state *const shared = created.get();
        created->worker = std::jthread{[shared](std::stop_token stop)
        {
            shared->run(stop);
        }};
        return std::unique_ptr<update_checker>{new update_checker{std::move(created)}};
    }

    update_state update_checker::current() const
    {
        std::scoped_lock lock{impl->mutex};
        return impl->shown;
    }

    void update_checker::check_now()
    {
        {
            std::scoped_lock lock{impl->mutex};
            impl->requested = true;
        }
        impl->wake.notify_all();
    }

    void update_checker::set_automatic(bool enabled)
    {
        {
            std::scoped_lock lock{impl->mutex};
            if (impl->shown.automatic == enabled)
            {
                return;
            }
            impl->shown.automatic = enabled;
            if (enabled)
            {
                // a day after the last answer, and not in the very moment the switch moves
                auto const now = std::chrono::system_clock::now();
                impl->due = std::max(now + std::chrono::seconds{5},
                                     impl->shown.checked ? *impl->shown.checked + check_interval : now);
            }
            ++impl->changes;
        }
        settings::store_number(setting_automatic, enabled ? 1U : 0U);
        log::info("update check: automatic {}", enabled ? "on (once a day)" : "off");
        impl->wake.notify_all();
        impl->announce(true);
    }
}

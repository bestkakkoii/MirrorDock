// SPDX-License-Identifier: MIT
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winver.h>

#include "uxplay_version.h"

module uxplay_update_install;

import std;
import uxplay_status;
import uxplay_log;
import uxplay_http_client;
import uxplay_update_check;
import uxplay_release_signature;
import uxplay_zip_archive;

namespace uxplay
{
    namespace
    {
        // the one folder a package consists of (build_scripts/package_zip.py)
        constexpr std::string_view package_folder = UXPLAY_PRODUCT_NAME "/";
        // in the program's folder: "new" is the unpacked release, "old" what it replaced
        constexpr wchar_t work_folder_name[] = L".update";
        constexpr wchar_t new_folder_name[] = L"new";
        constexpr wchar_t old_folder_name[] = L"old";
        // written when a release was put in place: the version, for the program that starts next
        constexpr wchar_t installed_note_name[] = L"installed";
        constexpr std::size_t longest_note = 32;
        constexpr std::size_t largest_package = 64 * 1024 * 1024;
        constexpr std::size_t largest_signature = 4096;
        constexpr std::size_t longest_name = 200;
        // for each step of a request (name, connection, sending, every piece received)
        constexpr auto request_patience = std::chrono::seconds{20};
        constexpr auto progress_interval = std::chrono::milliseconds{200};
        // the program that was replaced lets go of its file when it has ended
        constexpr int clearing_attempts = 20;
        constexpr auto clearing_interval = std::chrono::milliseconds{500};

        [[nodiscard]] std::filesystem::path program_folder()
        {
            std::wstring path(32768, L'\0');
            DWORD const length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (length == 0 || length >= path.size())
            {
                return {};
            }
            path.resize(length);
            return std::filesystem::path{path}.parent_path();
        }

        [[nodiscard]] std::string shown(std::filesystem::path const &path)
        {
            return utf16_to_utf8(path.wstring());
        }

        // Where an entry of the package goes below the folder it is unpacked
        // into.  Nothing for a name that must not be unpacked: one outside
        // the package's folder, one that would leave the folder it is
        // unpacked into, or one Windows would take for another name.
        [[nodiscard]] std::optional<std::filesystem::path> place_of(std::string_view name)
        {
            if (!name.starts_with(package_folder))
            {
                return std::nullopt;
            }
            name.remove_prefix(package_folder.size());
            if (name.empty() || name.size() > longest_name)
            {
                return std::nullopt;
            }
            std::filesystem::path place;
            while (!name.empty())
            {
                std::size_t const slash = name.find('/');
                std::string_view const part = name.substr(0, slash);
                name.remove_prefix(slash == std::string_view::npos ? name.size() : slash + 1);
                if (part.empty() || part == "." || part == ".." || part.ends_with(' ') || part.ends_with('.'))
                {
                    return std::nullopt;
                }
                for (char const character : part)
                {
                    if (static_cast<unsigned char>(character) < 0x20 ||
                        std::string_view{"\\:*?\"<>|"}.contains(character))
                    {
                        return std::nullopt;
                    }
                }
                place /= utf8_to_utf16(part);
            }
            return place;
        }

        // 0: written; else a Windows error code
        [[nodiscard]] DWORD write_file(std::filesystem::path const &path, std::span<std::uint8_t const> bytes)
        {
            HANDLE const file =
                CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                return GetLastError();
            }
            DWORD problem = ERROR_SUCCESS;
            std::size_t written = 0;
            while (written < bytes.size() && problem == ERROR_SUCCESS)
            {
                auto const piece = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - written, 1U << 24));
                DWORD done = 0;
                if (WriteFile(file, bytes.data() + written, piece, &done, nullptr) == FALSE)
                {
                    problem = GetLastError();
                }
                else if (done == 0)
                {
                    problem = ERROR_WRITE_FAULT;
                }
                written += done;
            }
            if (CloseHandle(file) == FALSE && problem == ERROR_SUCCESS)
            {
                problem = GetLastError();
            }
            return problem;
        }

        [[nodiscard]] std::span<std::uint8_t const> bytes_of(std::string_view text)
        {
            return {reinterpret_cast<std::uint8_t const *>(text.data()), text.size()};
        }

        // what the note of a completed update says; empty when it cannot be read
        [[nodiscard]] std::string read_note(std::filesystem::path const &file)
        {
            std::ifstream input{file, std::ios::binary};
            std::string text(longest_note, '\0');
            input.read(text.data(), static_cast<std::streamsize>(text.size()));
            text.resize(static_cast<std::size_t>(std::max<std::streamsize>(input.gcount(), 0)));
            return text;
        }

        // the version an executable's own description gives
        [[nodiscard]] std::optional<release_version> file_version(std::filesystem::path const &file)
        {
            DWORD ignored = 0;
            DWORD const size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
            if (size == 0)
            {
                return std::nullopt;
            }
            std::vector<std::uint8_t> description(size);
            void *found = nullptr;
            UINT length = 0;
            if (GetFileVersionInfoW(file.c_str(), 0, size, description.data()) == FALSE ||
                VerQueryValueW(description.data(), L"\\", &found, &length) == FALSE || found == nullptr ||
                length < sizeof(VS_FIXEDFILEINFO))
            {
                return std::nullopt;
            }
            auto const *const fixed = static_cast<VS_FIXEDFILEINFO const *>(found);
            return release_version{HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS),
                                   HIWORD(fixed->dwFileVersionLS)};
        }

        // A download on a thread of its own, so that the end of the program
        // does not wait for a server that does not answer: the installer stops
        // waiting and leaves that thread to end alone.  It uses nothing but
        // this block, which it owns together with the installer.
        struct pending_download
        {
            std::mutex mutex;
            std::condition_variable_any finished;
            std::optional<outcome<http_answer>> answer; // guarded by mutex
            std::atomic<bool> abandoned{false};
            std::atomic<std::uint64_t> received{0};
        };

        // nothing when the stop was requested first; progress: told now and then how many bytes arrived
        [[nodiscard]] std::optional<outcome<http_answer>> fetch(std::stop_token stop, std::string const &url,
                                                                std::size_t longest,
                                                                std::function<void(std::uint64_t)> const &progress)
        {
            auto const shared = std::make_shared<pending_download>();
            try
            {
                std::thread{[shared, url, longest]
                {
                    http_request request;
                    request.url = url;
                    // (what GitHub is told about this client: the same as by the update check)
                    request.user_agent = UXPLAY_PRODUCT_NAME "/" UXPLAY_VERSION_TEXT;
                    request.longest_answer = longest;
                    request.patience = request_patience;
                    request.cancelled = [shared]
                    {
                        return shared->abandoned.load();
                    };
                    request.received = [shared](std::size_t bytes)
                    {
                        shared->received.store(bytes);
                    };
                    auto answer = http_exchange(request);
                    {
                        std::scoped_lock lock{shared->mutex};
                        shared->answer = std::move(answer);
                    }
                    shared->finished.notify_all();
                }}.detach();
            }
            catch (std::system_error const &cause)
            {
                return std::unexpected(make_failure("thread for the download", cause.code().value(), cause.what()));
            }
            std::unique_lock lock{shared->mutex};
            for (;;)
            {
                if (shared->finished.wait_for(lock, stop, progress_interval, [&shared]
                    {
                        return shared->answer.has_value();
                    }))
                {
                    return std::move(shared->answer);
                }
                if (stop.stop_requested())
                {
                    shared->abandoned.store(true);
                    return std::nullopt;
                }
                if (progress)
                {
                    lock.unlock();
                    progress(shared->received.load());
                    lock.lock();
                }
            }
        }
    }

    struct update_installer::state
    {
        install_events events;
        std::filesystem::path folder; // the program's
        // set once in create(): an update was put in place before this start,
        // and this start is the first of the version it brought
        bool completed = false;
        bool updated = false;
        mutable std::mutex mutex;
        install_state now;     // guarded by mutex
        bool applying = false; // guarded by mutex: apply() runs, or ran and failed; cleared by prepare()
        std::jthread cleaner; // clears away what an update left, at the start
        std::jthread worker;  // last: stopped and joined before the others go

        template <class edit_type>
        void change(edit_type const &edit);
        void fail(install_problem problem, std::string detail);
        void clear(std::stop_token stop) const;
        void prepare(std::stop_token stop, published_release const &release);
    };

    // Edits the state under the lock and tells the owner without it.
    template <class edit_type>
    void update_installer::state::change(edit_type const &edit)
    {
        install_state copy;
        {
            std::scoped_lock lock{mutex};
            edit(now);
            copy = now;
        }
        if (events.on_change)
        {
            events.on_change(copy);
        }
    }

    // The update is given up: what was unpacked goes, the user is told why.
    void update_installer::state::fail(install_problem problem, std::string detail)
    {
        log::warning("update: {}", detail);
        std::error_code ignored;
        std::filesystem::remove_all(folder / work_folder_name / new_folder_name, ignored);
        change([problem, &detail](install_state &what)
        {
            what.phase = install_phase::failed;
            what.problem = problem;
            what.detail = detail;
        });
    }

    void update_installer::state::clear(std::stop_token stop) const
    {
        std::filesystem::path const work = folder / work_folder_name;
        std::error_code problem;
        if (!completed)
        {
            // No update was completed.  What was unpacked and never put in
            // place goes.  Files of the program that an update moved aside and
            // could not put back are the only copies there are: they stay
            // (remove() takes a folder only when it is empty), until the next
            // update clears the folder before it starts.
            std::filesystem::remove_all(work / new_folder_name, problem);
            std::filesystem::remove(work / old_folder_name, problem);
            std::filesystem::remove(work, problem);
            if (std::filesystem::exists(work, problem))
            {
                log::warning("update: {} still holds files of an update that was not completed", shown(work));
            }
            else
            {
                log::info("update: what an update that was not completed left was cleared away");
            }
            return;
        }
        std::mutex wait_mutex;
        std::condition_variable_any wait;
        for (int attempt = 0; attempt < clearing_attempts; ++attempt)
        {
            std::filesystem::remove_all(work, problem);
            if (!problem)
            {
                log::info("update: what the update left in the program's folder was cleared away");
                return;
            }
            std::unique_lock lock{wait_mutex};
            if (wait.wait_for(lock, stop, clearing_interval, []
                {
                    return false;
                }) ||
                stop.stop_requested())
            {
                return;
            }
        }
        log::warning("update: {} could not be cleared away ({}); the next start tries again", shown(work),
                     problem.message());
    }

    void update_installer::state::prepare(std::stop_token stop, published_release const &release)
    {
        std::string const version = release.version.text();
        log::info("update: fetching release {} to install it", version);
        if (!(release.version > running_version()))
        {
            fail(install_problem::other, std::format("release {} is not newer than this program ({})", version,
                                                     running_version().text()));
            return;
        }
        if (release.signature_url.empty())
        {
            fail(install_problem::no_signature,
                 std::format("release {} has no signature among its files: it cannot be checked, so it is not installed",
                             version));
            return;
        }
        // the folder has to take the files: found out before anything is fetched
        std::filesystem::path const work = folder / work_folder_name;
        std::filesystem::path const staging = work / new_folder_name;
        std::error_code problem;
        if (folder.empty())
        {
            fail(install_problem::folder, "the folder of the program is not known");
            return;
        }
        std::filesystem::remove_all(work, problem);
        if (!problem)
        {
            std::filesystem::create_directories(staging, problem);
        }
        if (problem)
        {
            fail(install_problem::folder, std::format("the program's folder cannot be written to: {} ({})",
                                                      shown(work), problem.message()));
            return;
        }

        // ---- the signature, then the package
        auto const fetched = [this, &stop](std::string const &url, std::size_t longest, std::string_view what,
                                           std::function<void(std::uint64_t)> const &progress)
            -> std::optional<std::vector<std::uint8_t>>
        {
            std::optional<outcome<http_answer>> answer = fetch(stop, url, longest, progress);
            if (!answer)
            {
                return std::nullopt; // the program ends
            }
            if (!*answer)
            {
                fail(install_problem::download, std::format("{} did not arrive: {}", what, answer->error().describe()));
                return std::nullopt;
            }
            if ((*answer)->status != 200)
            {
                fail(install_problem::download,
                     std::format("{} did not arrive: the server answered with HTTP status {}", what, (*answer)->status));
                return std::nullopt;
            }
            return std::move((*answer)->body);
        };
        std::optional<std::vector<std::uint8_t>> const signature_text =
            fetched(release.signature_url, largest_signature, "the signature of the package", {});
        if (!signature_text)
        {
            return;
        }
        std::optional<std::vector<std::uint8_t>> const package =
            fetched(release.package_url, largest_package, "the package", [this](std::uint64_t received)
            {
                bool moved = false;
                {
                    std::scoped_lock lock{mutex};
                    moved = now.received != received;
                }
                if (moved)
                {
                    change([received](install_state &what)
                    {
                        what.received = received;
                    });
                }
            });
        if (!package)
        {
            return;
        }
        if (release.package_bytes != 0 && package->size() != release.package_bytes)
        {
            fail(install_problem::download, std::format("the package has {} bytes, the release says {}",
                                                        package->size(), release.package_bytes));
            return;
        }
        change([&package](install_state &what)
        {
            what.phase = install_phase::checking;
            what.received = package->size();
        });

        // ---- is it the package the project published?
        auto const checked = check_release_signature(*package, *signature_text);
        if (!checked)
        {
            fail(install_problem::other, checked.error().describe());
            return;
        }
        if (!checked->well_formed)
        {
            fail(install_problem::not_genuine, "the signature file is not a signature (128 hexadecimal digits)");
            return;
        }
        if (!checked->fits)
        {
            fail(install_problem::not_genuine,
                 std::format("the signature does not fit the package (sha256 {}): it is not installed",
                             hexadecimal(checked->digest)));
            return;
        }
        log::info("update: the package of {} bytes carries the project's signature (sha256 {})", package->size(),
                  hexadecimal(checked->digest));

        // ---- unpack it beside the program
        auto archive = zip_archive::open(*package);
        if (!archive)
        {
            fail(install_problem::package, archive.error().describe());
            return;
        }
        std::size_t files = 0;
        for (std::size_t index = 0; index < archive->entries().size(); ++index)
        {
            if (stop.stop_requested())
            {
                return;
            }
            zip_entry const &entry = archive->entries()[index];
            if (entry.folder)
            {
                continue; // made when a file in it is written
            }
            std::optional<std::filesystem::path> const place = place_of(entry.name);
            if (!place)
            {
                fail(install_problem::package,
                     std::format("the package holds \"{}\", which is no file of the program", entry.name));
                return;
            }
            auto contents = archive->read(index);
            if (!contents)
            {
                fail(install_problem::package, contents.error().describe());
                return;
            }
            std::filesystem::path const target = staging / *place;
            std::filesystem::create_directories(target.parent_path(), problem);
            DWORD const written = problem ? static_cast<DWORD>(problem.value()) : write_file(target, *contents);
            if (written != ERROR_SUCCESS)
            {
                fail(install_problem::folder,
                     std::format("{} could not be written: {}", shown(target),
                                 make_win32_failure("writing a file of the release", written).describe()));
                return;
            }
            ++files;
        }

        // ---- the program in it must be the version the release says
        std::optional<release_version> const unpacked = file_version(staging / L"" UXPLAY_EXECUTABLE_NAME);
        if (!unpacked || *unpacked != release.version)
        {
            fail(install_problem::package,
                 std::format("the program in the package is {}, the release says {}",
                             unpacked ? std::format("version {}", unpacked->text())
                                      : std::string{"missing or names no version"},
                             version));
            return;
        }
        log::info("update: release {} is unpacked beside the program ({} files) and ready to be put in place", version,
                  files);
        change([](install_state &what)
        {
            what.phase = install_phase::ready;
        });
    }

    update_installer::update_installer(std::unique_ptr<state> created_state)
        : impl{std::move(created_state)}
    {
    }

    update_installer::~update_installer() = default;

    std::unique_ptr<update_installer> update_installer::create(install_events events)
    {
        auto created = std::make_unique<state>();
        created->events = std::move(events);
        created->folder = program_folder();
        std::error_code problem;
        std::filesystem::path const work = created->folder / work_folder_name;
        if (!created->folder.empty() && std::filesystem::exists(work, problem))
        {
            // The note of an update that was put in place names the version
            // it brought.  It is read once and removed at once, so that only
            // this start tells the user, whatever becomes of the rest.
            std::filesystem::path const note = work / installed_note_name;
            if (std::filesystem::exists(note, problem))
            {
                created->completed = true;
                created->updated = read_note(note) == running_version().text();
                std::filesystem::remove(note, problem);
                if (problem)
                {
                    log::warning("update: {} could not be removed ({})", shown(note), problem.message());
                }
            }
            if (created->updated)
            {
                log::info("update: this is the first start of {} after an update", UXPLAY_VERSION_TEXT);
            }
            state const *const shared = created.get();
            created->cleaner = std::jthread{[shared](std::stop_token stop)
            {
                shared->clear(stop);
            }};
        }
        return std::unique_ptr<update_installer>{new update_installer{std::move(created)}};
    }

    install_state update_installer::current() const
    {
        std::scoped_lock lock{impl->mutex};
        return impl->now;
    }

    bool update_installer::updated_at_start() const
    {
        return impl->updated;
    }

    void update_installer::prepare(published_release const &release)
    {
        install_state started;
        {
            std::scoped_lock lock{impl->mutex};
            install_phase const phase = impl->now.phase;
            if (phase == install_phase::downloading || phase == install_phase::checking ||
                phase == install_phase::installed ||
                (phase == install_phase::ready && (impl->applying || impl->now.version == release.version)))
            {
                return;
            }
            // Claimed under the lock: from here on apply() finds nothing
            // ready, so it cannot move files that the new attempt clears away.
            impl->applying = false;
            impl->now = install_state{};
            impl->now.phase = install_phase::downloading;
            impl->now.version = release.version;
            impl->now.expected = release.package_bytes;
            started = impl->now;
        }
        // (the thread of an earlier attempt has ended or is about to: its phase was idle, ready or failed)
        impl->cleaner = {};
        impl->worker = {};
        if (impl->events.on_change)
        {
            impl->events.on_change(started);
        }
        state *const shared = impl.get();
        impl->worker = std::jthread{[shared, release](std::stop_token stop)
        {
            shared->prepare(stop, release);
        }};
    }

    status update_installer::apply()
    {
        release_version version;
        {
            std::scoped_lock lock{impl->mutex};
            if (impl->now.phase != install_phase::ready || impl->applying)
            {
                return std::unexpected(make_failure("update", 0, "no release is ready to be put in place"));
            }
            // One thread puts it in place, whoever asks at the same moment
            // (the end of the stream, "Restart now", the end of the download).
            impl->applying = true;
            version = impl->now.version;
        }
        std::filesystem::path const &folder = impl->folder;
        std::filesystem::path const staging = folder / work_folder_name / new_folder_name;
        std::filesystem::path const replaced = folder / work_folder_name / old_folder_name;
        // what was moved, in order: undone from the end when a step fails
        struct moved_item
        {
            std::filesystem::path from;
            std::filesystem::path to;
        };
        std::vector<moved_item> moved;
        auto const give_up = [this, &moved](std::string_view step, std::filesystem::path const &item,
                                            std::error_code const &cause) -> status
        {
            failure const problem =
                make_failure(step, cause.value(), std::format("{}: {}", shown(item), cause.message()));
            bool restored = true;
            for (moved_item const &done : moved | std::views::reverse)
            {
                std::error_code undone;
                std::filesystem::rename(done.to, done.from, undone);
                restored = restored && !undone;
            }
            impl->fail(install_problem::folder,
                       std::format("{}; {}", problem.describe(),
                                   restored ? "every file is where it was"
                                            : "not every file could be put back: download the release and unpack "
                                              "it by hand"));
            if (restored)
            {
                // nothing is left of the attempt (both are empty now)
                std::error_code ignored;
                std::filesystem::remove(impl->folder / work_folder_name / old_folder_name, ignored);
                std::filesystem::remove(impl->folder / work_folder_name, ignored);
            }
            return std::unexpected(problem);
        };
        std::error_code problem;
        std::filesystem::create_directories(replaced, problem);
        if (problem)
        {
            return give_up("making room for the replaced files", replaced, problem);
        }
        // each thing a release has at its top: the program, its guides, its licences, ...
        std::vector<std::filesystem::path> names;
        for (std::filesystem::directory_iterator item{staging, problem}, end; !problem && item != end;
             item.increment(problem))
        {
            names.push_back(item->path().filename());
        }
        if (problem || names.empty())
        {
            return give_up("reading the unpacked release", staging,
                           problem ? problem : std::make_error_code(std::errc::no_such_file_or_directory));
        }
        for (std::filesystem::path const &name : names)
        {
            std::filesystem::path const target = folder / name;
            if (std::filesystem::exists(std::filesystem::symlink_status(target, problem)))
            {
                // (a running executable can be renamed, and goes on running)
                std::filesystem::rename(target, replaced / name, problem);
                if (problem)
                {
                    return give_up("moving a file of the running program aside", target, problem);
                }
                moved.push_back(moved_item{target, replaced / name});
            }
            std::filesystem::rename(staging / name, target, problem);
            if (problem)
            {
                return give_up("putting a file of the release in place", target, problem);
            }
            moved.push_back(moved_item{staging / name, target});
        }
        // the note for the program that starts next: it says "updated" once, and clears the rest away
        std::string const note = version.text();
        if (DWORD const written = write_file(folder / work_folder_name / installed_note_name, bytes_of(note));
            written != ERROR_SUCCESS)
        {
            log::warning("update: the note for the next start could not be written ({}): the new program will not "
                         "say that it was updated",
                         make_win32_failure("writing the note of the update", written).describe());
        }
        log::info("update: release {} is in place of {} ({} top-level items)", version.text(),
                  running_version().text(), names.size());
        impl->change([](install_state &what)
        {
            what.phase = install_phase::installed;
        });
        return {};
    }

    std::filesystem::path update_installer::restart_target()
    {
        std::filesystem::path const folder = program_folder();
        return folder.empty() ? std::filesystem::path{} : folder / L"" UXPLAY_EXECUTABLE_NAME;
    }
}

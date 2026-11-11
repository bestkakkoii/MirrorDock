// SPDX-License-Identifier: GPL-3.0-or-later
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <wincrypt.h>
#include <winhttp.h>

module uxplay_http_client;

import std;
import uxplay_status;

namespace uxplay
{
    namespace
    {
        struct handle_closer
        {
            void operator()(HINTERNET handle) const
            {
                WinHttpCloseHandle(handle);
            }
        };

        using internet_handle = std::unique_ptr<void, handle_closer>;

        [[nodiscard]] failure http_failure(std::string_view stage)
        {
            return make_win32_failure(stage, GetLastError());
        }

        struct store_closer
        {
            void operator()(HCERTSTORE store) const
            {
                CertCloseStore(store, 0);
            }
        };

        // what the status callback needs to check the server's certificate
        struct root_check
        {
            std::span<std::uint8_t const> root;
            std::wstring host;
            internet_handle *exchange = nullptr; // closed when the certificate does not pass
            bool done = false;
            std::optional<failure> problem;
        };

        // Does the certificate belong to `check.host`, is it valid today, and
        // does its chain end in the one root that is trusted here?
        [[nodiscard]] std::optional<failure> chain_problem(PCCERT_CONTEXT certificate, root_check const &check)
        {
            std::unique_ptr<void, store_closer> const roots{CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr)};
            if (!roots || CertAddEncodedCertificateToStore(roots.get(), X509_ASN_ENCODING, check.root.data(),
                                                           static_cast<DWORD>(check.root.size()),
                                                           CERT_STORE_ADD_ALWAYS, nullptr) == FALSE)
            {
                return http_failure("the root certificate that is trusted for this server");
            }
            CERT_CHAIN_ENGINE_CONFIG configuration{};
            configuration.cbSize = sizeof(configuration);
            configuration.hExclusiveRoot = roots.get();
            HCERTCHAINENGINE engine = nullptr;
            if (CertCreateCertificateChainEngine(&configuration, &engine) == FALSE)
            {
                return http_failure("CertCreateCertificateChainEngine");
            }
            CERT_CHAIN_PARA parameters{};
            parameters.cbSize = sizeof(parameters);
            PCCERT_CHAIN_CONTEXT chain = nullptr;
            std::optional<failure> problem;
            // (the certificates the server sent along are in the store of its own certificate)
            if (CertGetCertificateChain(engine, certificate, nullptr, certificate->hCertStore, &parameters, 0, nullptr,
                                        &chain) == FALSE)
            {
                problem = http_failure("CertGetCertificateChain");
            }
            else
            {
                std::wstring host = check.host;
                SSL_EXTRA_CERT_CHAIN_POLICY_PARA server{};
                server.cbSize = sizeof(server);
                server.dwAuthType = AUTHTYPE_SERVER;
                server.pwszServerName = host.data();
                CERT_CHAIN_POLICY_PARA policy{};
                policy.cbSize = sizeof(policy);
                policy.pvExtraPolicyPara = &server;
                CERT_CHAIN_POLICY_STATUS result{};
                result.cbSize = sizeof(result);
                if (CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &result) == FALSE)
                {
                    problem = http_failure("CertVerifyCertificateChainPolicy");
                }
                else if (result.dwError != 0)
                {
                    problem = make_win32_failure("the server's certificate", result.dwError);
                }
                CertFreeCertificateChain(chain);
            }
            CertFreeCertificateChainEngine(engine);
            return problem;
        }

        // Called by WinHTTP when the connection is secured and the request is
        // about to go out: the moment to look at whom it would go to.
        void CALLBACK before_sending(HINTERNET handle, DWORD_PTR context, DWORD status, LPVOID, DWORD)
        {
            if (status != WINHTTP_CALLBACK_STATUS_SENDING_REQUEST || context == 0)
            {
                return;
            }
            auto *const check = reinterpret_cast<root_check *>(context);
            if (std::exchange(check->done, true))
            {
                return;
            }
            PCCERT_CONTEXT certificate = nullptr;
            DWORD size = sizeof(certificate);
            if (WinHttpQueryOption(handle, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &certificate, &size) == FALSE ||
                certificate == nullptr)
            {
                check->problem = http_failure("the server's certificate");
            }
            else
            {
                check->problem = chain_problem(certificate, *check);
                CertFreeCertificateContext(certificate);
            }
            if (check->problem)
            {
                check->exchange->reset(); // nothing is sent to a server that is not the expected one
            }
        }
    }

    outcome<http_answer> http_exchange(http_request const &request)
    {
        std::wstring const url = utf8_to_utf16(request.url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        std::wstring host(256, L'\0');
        std::wstring path(2048, L'\0');
        parts.lpszHostName = host.data();
        parts.dwHostNameLength = static_cast<DWORD>(host.size());
        parts.lpszUrlPath = path.data();
        parts.dwUrlPathLength = static_cast<DWORD>(path.size());
        if (WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) == FALSE)
        {
            return std::unexpected(http_failure("WinHttpCrackUrl"));
        }
        host.resize(parts.dwHostNameLength);
        path.resize(parts.dwUrlPathLength);
        bool const secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

        std::wstring const agent = utf8_to_utf16(request.user_agent);
        internet_handle const session{WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
        if (!session)
        {
            return std::unexpected(http_failure("WinHttpOpen"));
        }
        auto const patience = static_cast<int>(
            std::clamp<std::chrono::milliseconds::rep>(request.patience.count(), 1000, 600'000));
        WinHttpSetTimeouts(session.get(), patience, patience, patience, patience);
        internet_handle const connection{WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0)};
        if (!connection)
        {
            return std::unexpected(http_failure("WinHttpConnect"));
        }
        bool const with_body = !request.content_type.empty();
        internet_handle exchange{
            WinHttpOpenRequest(connection.get(), with_body ? L"POST" : L"GET", path.c_str(), nullptr,
                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)};
        if (!exchange)
        {
            return std::unexpected(http_failure("WinHttpOpenRequest"));
        }
        root_check check;
        check.root = request.only_trusted_root;
        check.host = host;
        check.exchange = &exchange;
        bool const own_root = !request.only_trusted_root.empty();
        if (own_root)
        {
            if (!secure)
            {
                return std::unexpected(
                    make_failure("HTTP request", 0, "a root certificate is named for a request that is not https"));
            }
            // WinHTTP lets the unknown root pass; the callback then checks the whole chain against it
            DWORD security = SECURITY_FLAG_IGNORE_UNKNOWN_CA;
            DWORD redirection = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            if (WinHttpSetOption(exchange.get(), WINHTTP_OPTION_SECURITY_FLAGS, &security, sizeof(security)) == FALSE ||
                WinHttpSetOption(exchange.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirection, sizeof(redirection)) ==
                    FALSE ||
                WinHttpSetStatusCallback(exchange.get(), &before_sending, WINHTTP_CALLBACK_FLAG_SEND_REQUEST, 0) ==
                    WINHTTP_INVALID_STATUS_CALLBACK)
            {
                return std::unexpected(http_failure("preparation of the certificate check"));
            }
        }
        std::wstring headers = L"Cache-Control: no-cache\r\n";
        if (with_body)
        {
            headers += std::format(L"Content-Type: {}\r\n", utf8_to_utf16(request.content_type));
        }
        // (the body is not changed by the call; the interface has no const)
        void *const body = request.body.empty() ? nullptr : const_cast<std::uint8_t *>(request.body.data());
        BOOL const sent =
            WinHttpSendRequest(exchange.get(), headers.c_str(), static_cast<DWORD>(headers.size()), body,
                               static_cast<DWORD>(request.body.size()), static_cast<DWORD>(request.body.size()),
                               own_root ? reinterpret_cast<DWORD_PTR>(&check) : 0);
        if (check.problem)
        {
            return std::unexpected(make_failure(check.problem->stage, check.problem->code,
                                                std::format("{} (nothing was sent to it)", check.problem->message)));
        }
        if (sent == FALSE)
        {
            return std::unexpected(http_failure("WinHttpSendRequest"));
        }
        if (own_root && !check.done)
        {
            return std::unexpected(
                make_failure("the server's certificate", 0, "it could not be checked before the request went out"));
        }
        if (WinHttpReceiveResponse(exchange.get(), nullptr) == FALSE)
        {
            return std::unexpected(http_failure("WinHttpReceiveResponse"));
        }
        DWORD status_code = 0;
        DWORD status_bytes = sizeof(status_code);
        if (WinHttpQueryHeaders(exchange.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_bytes,
                                WINHTTP_NO_HEADER_INDEX) == FALSE)
        {
            return std::unexpected(http_failure("WinHttpQueryHeaders"));
        }
        http_answer answer;
        answer.status = static_cast<int>(status_code);
        for (;;)
        {
            if (request.cancelled && request.cancelled())
            {
                return std::unexpected(make_failure("HTTP request", 0, "ended here before the answer was complete"));
            }
            DWORD available = 0;
            if (WinHttpQueryDataAvailable(exchange.get(), &available) == FALSE)
            {
                return std::unexpected(http_failure("WinHttpQueryDataAvailable"));
            }
            if (available == 0)
            {
                break;
            }
            if (answer.body.size() + available > request.longest_answer)
            {
                return std::unexpected(make_failure("HTTP request", static_cast<std::int64_t>(request.longest_answer),
                                                    "the answer is longer than this many bytes"));
            }
            std::size_t const filled = answer.body.size();
            answer.body.resize(filled + available);
            DWORD taken = 0;
            if (WinHttpReadData(exchange.get(), answer.body.data() + filled, available, &taken) == FALSE)
            {
                return std::unexpected(http_failure("WinHttpReadData"));
            }
            answer.body.resize(filled + taken);
            if (taken == 0)
            {
                break;
            }
        }
        return answer;
    }
}

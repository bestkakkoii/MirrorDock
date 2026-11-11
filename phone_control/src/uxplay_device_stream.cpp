// SPDX-License-Identifier: GPL-3.0-or-later
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

module uxplay_device_stream;

import std;
import uxplay_status;
import uxplay_byte_stream;
import uxplay_usbmux;

namespace uxplay
{
    namespace
    {
        // a wait for the socket is cut into pieces of this length, so that
        // shut_down() is noticed without a second socket or event
        constexpr long wait_slice_microseconds = 100'000;
        // The phone takes what is written at once; when it has not for this
        // long, it is gone or hangs.
        constexpr std::chrono::milliseconds write_patience = std::chrono::seconds{20};

        [[nodiscard]] failure openssl_failure(std::string_view stage)
        {
            unsigned long const code = ERR_get_error();
            std::array<char, 256> text{};
            ERR_error_string_n(code, text.data(), text.size());
            ERR_clear_error();
            return make_failure(stage, static_cast<std::int64_t>(code), text.data());
        }

        struct bio_closer
        {
            void operator()(BIO *bio) const
            {
                BIO_free(bio);
            }
        };
    }

    struct device_stream::state
    {
        device_socket socket;
        std::atomic<bool> stopped{false};
        // how long a read waits for the phone, in milliseconds
        std::atomic<std::chrono::milliseconds::rep> read_patience{no_read_limit.count()};
        // TLS (null while plain).  An SSL object serves one call at a time:
        // reads and writes take turns under this mutex, and wait for the
        // socket outside of it.
        std::mutex tls_mutex;
        SSL_CTX *context = nullptr;
        SSL *tls = nullptr;

        [[nodiscard]] SOCKET raw() const
        {
            return static_cast<SOCKET>(socket.native());
        }

        // until the socket can be read (or written); a failure when the
        // stream was shut down or the phone kept this end waiting for too long
        [[nodiscard]] status wait(bool for_reading, std::string_view stage) const
        {
            std::chrono::milliseconds const patience =
                for_reading ? std::chrono::milliseconds{read_patience.load()} : write_patience;
            auto const began = std::chrono::steady_clock::now();
            for (;;)
            {
                if (stopped.load())
                {
                    return std::unexpected(make_failure(stage, 0, "the connection was closed here"));
                }
                fd_set set;
                FD_ZERO(&set);
                FD_SET(raw(), &set);
                timeval slice{};
                slice.tv_usec = wait_slice_microseconds;
                int const ready = select(0, for_reading ? &set : nullptr, for_reading ? nullptr : &set, nullptr, &slice);
                if (ready > 0)
                {
                    return {};
                }
                if (ready < 0)
                {
                    return std::unexpected(make_win32_failure(stage, static_cast<unsigned long>(WSAGetLastError())));
                }
                if (patience != no_read_limit && std::chrono::steady_clock::now() - began >= patience)
                {
                    return std::unexpected(make_failure(
                        stage, 0,
                        std::format("{} within {} ms",
                                    for_reading ? "the phone did not answer" : "the phone did not take the data",
                                    patience.count())));
                }
            }
        }

        // what an SSL call wants before it can go on; a failure for everything else
        [[nodiscard]] status after_tls_call(int result, std::string_view stage)
        {
            int const reason = SSL_get_error(tls, result);
            switch (reason)
            {
            case SSL_ERROR_WANT_READ:
                return wait(true, stage);
            case SSL_ERROR_WANT_WRITE:
                return wait(false, stage);
            case SSL_ERROR_ZERO_RETURN:
                return std::unexpected(make_failure(stage, 0, "the phone closed the connection"));
            case SSL_ERROR_SYSCALL:
            {
                int const socket_error = WSAGetLastError();
                ERR_clear_error();
                if (socket_error == 0)
                {
                    return std::unexpected(make_failure(stage, 0, "the phone closed the connection"));
                }
                return std::unexpected(make_win32_failure(stage, static_cast<unsigned long>(socket_error)));
            }
            default:
                return std::unexpected(openssl_failure(stage));
            }
        }
    };

    device_stream::device_stream(device_socket socket)
        : impl{std::make_unique<state>()}
    {
        impl->socket = std::move(socket);
        // waits are done with select (see state::wait): calls never block in the socket
        u_long non_blocking = 1;
        ioctlsocket(impl->raw(), static_cast<long>(FIONBIO), &non_blocking);
    }

    device_stream::~device_stream()
    {
        if (impl->tls != nullptr)
        {
            SSL_free(impl->tls);
        }
        if (impl->context != nullptr)
        {
            SSL_CTX_free(impl->context);
        }
    }

    status device_stream::start_tls(std::span<std::uint8_t const> certificate_pem,
                                    std::span<std::uint8_t const> private_key_pem)
    {
        if (impl->tls != nullptr)
        {
            return std::unexpected(make_failure("start_tls", 0, "the connection is encrypted already"));
        }
        impl->context = SSL_CTX_new(TLS_client_method());
        if (impl->context == nullptr)
        {
            return std::unexpected(openssl_failure("SSL_CTX_new"));
        }
        // The phone's side of this TLS is as old as the pairing scheme (its
        // certificate is signed with what current defaults refuse), and its
        // identity is established by the pairing, not by a certificate chain.
        SSL_CTX_set_security_level(impl->context, 0);
        SSL_CTX_set_min_proto_version(impl->context, TLS1_VERSION);
        SSL_CTX_set_verify(impl->context, SSL_VERIFY_NONE, nullptr);

        std::unique_ptr<BIO, bio_closer> const certificate_source{
            BIO_new_mem_buf(certificate_pem.data(), static_cast<int>(certificate_pem.size()))};
        std::unique_ptr<BIO, bio_closer> const key_source{
            BIO_new_mem_buf(private_key_pem.data(), static_cast<int>(private_key_pem.size()))};
        X509 *const certificate = PEM_read_bio_X509(certificate_source.get(), nullptr, nullptr, nullptr);
        EVP_PKEY *const key = PEM_read_bio_PrivateKey(key_source.get(), nullptr, nullptr, nullptr);
        bool const loaded = certificate != nullptr && key != nullptr &&
                            SSL_CTX_use_certificate(impl->context, certificate) == 1 &&
                            SSL_CTX_use_PrivateKey(impl->context, key) == 1;
        failure const load_problem = loaded ? failure{} : openssl_failure("pairing certificate and key");
        X509_free(certificate);
        EVP_PKEY_free(key);
        if (!loaded)
        {
            return std::unexpected(load_problem);
        }

        impl->tls = SSL_new(impl->context);
        if (impl->tls == nullptr)
        {
            return std::unexpected(openssl_failure("SSL_new"));
        }
        SSL_set_fd(impl->tls, static_cast<int>(impl->raw()));
        // a write may be continued from a buffer at another address (write_all retries with the same data)
        SSL_set_mode(impl->tls, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        for (;;)
        {
            int const result = SSL_connect(impl->tls);
            if (result == 1)
            {
                return {};
            }
            if (status const waited = impl->after_tls_call(result, "TLS handshake with the phone"); !waited)
            {
                return waited;
            }
        }
    }

    outcome<std::size_t> device_stream::read_some(std::span<std::uint8_t> buffer)
    {
        if (buffer.empty())
        {
            return std::size_t{0};
        }
        int const wanted = static_cast<int>(std::min<std::size_t>(buffer.size(), 1 << 20));
        for (;;)
        {
            if (impl->tls != nullptr)
            {
                // what the call needs is decided under the lock; the waiting
                // for the socket happens outside of it, so that a write can
                // go on meanwhile
                bool for_reading = true;
                {
                    std::scoped_lock lock{impl->tls_mutex};
                    int const count = SSL_read(impl->tls, buffer.data(), wanted);
                    if (count > 0)
                    {
                        return static_cast<std::size_t>(count);
                    }
                    int const reason = SSL_get_error(impl->tls, count);
                    if (reason == SSL_ERROR_WANT_WRITE)
                    {
                        for_reading = false;
                    }
                    else if (reason != SSL_ERROR_WANT_READ)
                    {
                        return std::unexpected(impl->after_tls_call(count, "read from the phone").error());
                    }
                }
                if (status const ready = impl->wait(for_reading, "read from the phone"); !ready)
                {
                    return std::unexpected(ready.error());
                }
                continue;
            }
            int const count = recv(impl->raw(), reinterpret_cast<char *>(buffer.data()), wanted, 0);
            if (count > 0)
            {
                return static_cast<std::size_t>(count);
            }
            if (count == 0)
            {
                return std::unexpected(make_failure("read from the phone", 0, "the phone closed the connection"));
            }
            int const error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK)
            {
                return std::unexpected(make_win32_failure("read from the phone", static_cast<unsigned long>(error)));
            }
            if (status const ready = impl->wait(true, "read from the phone"); !ready)
            {
                return std::unexpected(ready.error());
            }
        }
    }

    status device_stream::write_all(std::span<std::uint8_t const> data)
    {
        std::size_t written = 0;
        while (written < data.size())
        {
            int const wanted = static_cast<int>(std::min<std::size_t>(data.size() - written, 1 << 20));
            if (impl->tls != nullptr)
            {
                bool for_reading = false;
                {
                    std::scoped_lock lock{impl->tls_mutex};
                    int const count = SSL_write(impl->tls, data.data() + written, wanted);
                    if (count > 0)
                    {
                        written += static_cast<std::size_t>(count);
                        continue;
                    }
                    int const reason = SSL_get_error(impl->tls, count);
                    if (reason == SSL_ERROR_WANT_READ)
                    {
                        for_reading = true;
                    }
                    else if (reason != SSL_ERROR_WANT_WRITE)
                    {
                        return std::unexpected(impl->after_tls_call(count, "write to the phone").error());
                    }
                }
                if (status const ready = impl->wait(for_reading, "write to the phone"); !ready)
                {
                    return ready;
                }
                continue;
            }
            int const count = send(impl->raw(), reinterpret_cast<char const *>(data.data() + written), wanted, 0);
            if (count > 0)
            {
                written += static_cast<std::size_t>(count);
                continue;
            }
            int const error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK)
            {
                return std::unexpected(make_win32_failure("write to the phone", static_cast<unsigned long>(error)));
            }
            if (status const ready = impl->wait(false, "write to the phone"); !ready)
            {
                return ready;
            }
        }
        return {};
    }

    void device_stream::shut_down()
    {
        impl->stopped.store(true);
        shutdown(impl->raw(), SD_BOTH);
    }

    void device_stream::set_read_patience(std::chrono::milliseconds patience)
    {
        impl->read_patience.store(patience.count());
    }
}

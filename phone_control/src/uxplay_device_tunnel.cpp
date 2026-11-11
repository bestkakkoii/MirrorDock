// SPDX-License-Identifier: GPL-3.0-or-later
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

module uxplay_device_tunnel;

import std;
import uxplay_status;
import uxplay_log;
import uxplay_byte_stream;
import uxplay_lockdown;

namespace uxplay
{
    namespace
    {
        constexpr std::string_view tunnel_service_name = "com.apple.internal.devicecompute.CoreDeviceProxy";
        constexpr std::string_view handshake_magic = "CDTunnel";
        constexpr std::uint32_t requested_mtu = 16000;

        constexpr std::size_t ipv6_header_bytes = 40;
        constexpr std::size_t tcp_header_bytes = 20;
        constexpr std::size_t udp_header_bytes = 8;
        constexpr std::uint8_t protocol_tcp = 6;
        constexpr std::uint8_t protocol_udp = 17;
        constexpr std::uint8_t flag_fin = 0x01;
        constexpr std::uint8_t flag_syn = 0x02;
        constexpr std::uint8_t flag_rst = 0x04;
        constexpr std::uint8_t flag_psh = 0x08;
        constexpr std::uint8_t flag_ack = 0x10;

        // what a connection holds for its reader before the phone has to wait
        constexpr std::size_t receive_capacity = 1 << 20;
        // this end announces its window in units of 2^5 bytes (up to 2 MB)
        constexpr unsigned offered_window_shift = 5;
        constexpr auto connect_patience = std::chrono::seconds{8};
        // a write waits this long for the phone to take earlier data
        constexpr auto send_patience = std::chrono::seconds{20};
        // ports of this end are taken from here upwards
        constexpr std::uint16_t first_local_port = 49152;

        using address_bytes = std::array<std::uint8_t, 16>;

        // sequence numbers wrap: "a comes before b"
        [[nodiscard]] constexpr bool before(std::uint32_t a, std::uint32_t b)
        {
            return static_cast<std::int32_t>(a - b) < 0;
        }

        void put_be16(std::uint8_t *target, std::uint16_t value)
        {
            target[0] = static_cast<std::uint8_t>(value >> 8);
            target[1] = static_cast<std::uint8_t>(value);
        }

        void put_be32(std::uint8_t *target, std::uint32_t value)
        {
            target[0] = static_cast<std::uint8_t>(value >> 24);
            target[1] = static_cast<std::uint8_t>(value >> 16);
            target[2] = static_cast<std::uint8_t>(value >> 8);
            target[3] = static_cast<std::uint8_t>(value);
        }

        [[nodiscard]] std::uint16_t get_be16(std::uint8_t const *source)
        {
            return static_cast<std::uint16_t>((source[0] << 8) | source[1]);
        }

        [[nodiscard]] std::uint32_t get_be32(std::uint8_t const *source)
        {
            return (static_cast<std::uint32_t>(source[0]) << 24) | (static_cast<std::uint32_t>(source[1]) << 16) |
                   (static_cast<std::uint32_t>(source[2]) << 8) | static_cast<std::uint32_t>(source[3]);
        }

        // Internet checksum of an upper-layer packet with its IPv6 pseudo-header.
        [[nodiscard]] std::uint16_t upper_layer_checksum(address_bytes const &source, address_bytes const &destination,
                                                         std::uint8_t protocol, std::span<std::uint8_t const> packet)
        {
            std::uint32_t sum = 0;
            auto const add = [&sum](std::span<std::uint8_t const> bytes)
            {
                std::size_t index = 0;
                for (; index + 1 < bytes.size(); index += 2)
                {
                    sum += static_cast<std::uint32_t>((bytes[index] << 8) | bytes[index + 1]);
                }
                if (index < bytes.size())
                {
                    sum += static_cast<std::uint32_t>(bytes[index] << 8);
                }
            };
            add(source);
            add(destination);
            std::array<std::uint8_t, 8> tail{};
            put_be32(tail.data(), static_cast<std::uint32_t>(packet.size()));
            tail[7] = protocol;
            add(tail);
            add(packet);
            while ((sum >> 16) != 0)
            {
                sum = (sum & 0xFFFF) + (sum >> 16);
            }
            return static_cast<std::uint16_t>(~sum);
        }

        // "key":"text" in the small JSON object of the handshake
        [[nodiscard]] std::optional<std::string> json_text(std::string_view object, std::string_view key)
        {
            std::string const pattern = std::format("\"{}\":\"", key);
            std::size_t const start = object.find(pattern);
            if (start == std::string_view::npos)
            {
                return std::nullopt;
            }
            std::size_t const from = start + pattern.size();
            std::size_t const end = object.find('"', from);
            if (end == std::string_view::npos)
            {
                return std::nullopt;
            }
            return std::string{object.substr(from, end - from)};
        }

        // "key":number
        [[nodiscard]] std::optional<std::uint32_t> json_number(std::string_view object, std::string_view key)
        {
            std::string const pattern = std::format("\"{}\":", key);
            std::size_t const start = object.find(pattern);
            if (start == std::string_view::npos)
            {
                return std::nullopt;
            }
            std::string_view const rest = object.substr(start + pattern.size());
            std::uint32_t value = 0;
            auto const [end, error] = std::from_chars(rest.data(), rest.data() + rest.size(), value);
            if (error != std::errc{} || end == rest.data())
            {
                return std::nullopt;
            }
            return value;
        }

        [[nodiscard]] std::optional<address_bytes> parse_address(std::string const &text)
        {
            in6_addr parsed{};
            if (inet_pton(AF_INET6, text.c_str(), &parsed) != 1)
            {
                return std::nullopt;
            }
            address_bytes bytes{};
            std::memcpy(bytes.data(), &parsed, bytes.size());
            return bytes;
        }

        // one TCP connection inside the tunnel (all fields guarded by the tunnel's mutex)
        struct connection
        {
            std::uint16_t local_port = 0;
            std::uint16_t remote_port = 0;
            bool connecting = true;
            bool reset = false;         // by the phone
            bool closed_here = false;   // shut_down() or the owner is gone
            bool finish_sent = false;   // this end sent its FIN
            bool peer_finished = false; // the phone sent its FIN: no more data comes
            std::uint32_t send_next = 0;
            std::uint32_t send_unacknowledged = 0;
            std::uint32_t receive_next = 0;
            std::uint32_t peer_window = 0;     // bytes the phone will take beyond send_unacknowledged
            unsigned peer_window_shift = 0;
            unsigned own_window_shift = 0;     // 0 unless the phone scales as well
            std::uint32_t peer_segment = 1220; // the largest segment the phone takes
            std::vector<std::uint8_t> received;
            std::size_t received_offset = 0; // what the reader has taken already
            std::chrono::milliseconds read_patience = no_read_limit; // how long a read waits for data
            std::uint32_t announced_window = 0;
            std::condition_variable changed;
            std::mutex write_mutex; // one write_all at a time

            [[nodiscard]] std::size_t buffered() const
            {
                return received.size() - received_offset;
            }

            // the window field for the next segment of this end
            [[nodiscard]] std::uint16_t window_field() const
            {
                std::size_t const room = buffered() >= receive_capacity ? 0 : receive_capacity - buffered();
                return static_cast<std::uint16_t>(std::min<std::size_t>(room >> own_window_shift, 0xFFFF));
            }
        };
    }

    struct device_tunnel::state
    {
        std::unique_ptr<byte_stream> link;
        address_bytes local{};
        address_bytes phone{};
        std::string local_text;
        std::string phone_text;
        std::uint16_t directory_port = 0;
        std::uint32_t mtu = 1280;

        std::mutex mutex;
        // by (port of this end, port of the phone)
        std::map<std::pair<std::uint16_t, std::uint16_t>, std::shared_ptr<connection>> connections;
        std::set<std::uint16_t> discard_ports;
        std::uint16_t next_port = first_local_port;
        std::atomic<std::uint64_t> discarded{0};
        std::atomic<bool> broken{false};
        std::string break_reason; // guarded by mutex

        std::mutex send_mutex; // one packet at a time on the link
        std::thread reader;

        [[nodiscard]] status send_segment(std::uint16_t local_port, std::uint16_t remote_port, std::uint32_t sequence,
                                          std::uint32_t acknowledgement, std::uint8_t flags, std::uint16_t window,
                                          std::span<std::uint8_t const> options, std::span<std::uint8_t const> data);
        void handle_tcp(std::span<std::uint8_t const> segment);
        void read_loop();
        void mark_broken(std::string reason);
    };

    status device_tunnel::state::send_segment(std::uint16_t local_port, std::uint16_t remote_port,
                                              std::uint32_t sequence, std::uint32_t acknowledgement, std::uint8_t flags,
                                              std::uint16_t window, std::span<std::uint8_t const> options,
                                              std::span<std::uint8_t const> data)
    {
        std::size_t const header = tcp_header_bytes + options.size();
        std::vector<std::uint8_t> packet(ipv6_header_bytes + header + data.size());
        std::uint8_t *const ip = packet.data();
        ip[0] = 0x60; // version 6
        put_be16(ip + 4, static_cast<std::uint16_t>(header + data.size()));
        ip[6] = protocol_tcp;
        ip[7] = 64; // hop limit
        std::memcpy(ip + 8, local.data(), local.size());
        std::memcpy(ip + 24, phone.data(), phone.size());
        std::uint8_t *const tcp = ip + ipv6_header_bytes;
        put_be16(tcp, local_port);
        put_be16(tcp + 2, remote_port);
        put_be32(tcp + 4, sequence);
        put_be32(tcp + 8, acknowledgement);
        tcp[12] = static_cast<std::uint8_t>((header / 4) << 4);
        tcp[13] = flags;
        put_be16(tcp + 14, window);
        if (!options.empty())
        {
            std::memcpy(tcp + tcp_header_bytes, options.data(), options.size());
        }
        if (!data.empty())
        {
            std::memcpy(tcp + header, data.data(), data.size());
        }
        put_be16(tcp + 16, upper_layer_checksum(local, phone, protocol_tcp,
                                                std::span<std::uint8_t const>{tcp, header + data.size()}));
        std::scoped_lock lock{send_mutex};
        return link->write_all(packet);
    }

    void device_tunnel::state::mark_broken(std::string reason)
    {
        std::scoped_lock lock{mutex};
        if (!broken.exchange(true))
        {
            break_reason = std::move(reason);
        }
        for (auto const &entry : connections)
        {
            entry.second->changed.notify_all();
        }
    }

    // One TCP segment from the phone (reader thread).
    void device_tunnel::state::handle_tcp(std::span<std::uint8_t const> segment)
    {
        if (segment.size() < tcp_header_bytes)
        {
            return;
        }
        std::uint16_t const remote_port = get_be16(segment.data());
        std::uint16_t const local_port = get_be16(segment.data() + 2);
        std::uint32_t const sequence = get_be32(segment.data() + 4);
        std::uint32_t const acknowledgement = get_be32(segment.data() + 8);
        std::size_t const header = static_cast<std::size_t>(segment[12] >> 4) * 4;
        std::uint8_t const flags = segment[13];
        std::uint16_t const window = get_be16(segment.data() + 14);
        if (header < tcp_header_bytes || header > segment.size())
        {
            return;
        }
        std::span<std::uint8_t const> const options = segment.subspan(tcp_header_bytes, header - tcp_header_bytes);
        std::span<std::uint8_t const> const payload = segment.subspan(header);

        // what this end answers, decided under the lock and sent after it
        bool answer = false;
        std::uint32_t answer_sequence = 0;
        std::uint32_t answer_acknowledgement = 0;
        std::uint16_t answer_window = 0;
        {
            std::scoped_lock lock{mutex};
            auto const found = connections.find({local_port, remote_port});
            if (found == connections.end())
            {
                return; // nothing of ours
            }
            connection &link_state = *found->second;
            if ((flags & flag_rst) != 0)
            {
                link_state.reset = true;
                link_state.connecting = false;
                link_state.changed.notify_all();
                if (link_state.closed_here)
                {
                    connections.erase(found);
                }
                return;
            }
            if (link_state.connecting)
            {
                if ((flags & (flag_syn | flag_ack)) != (flag_syn | flag_ack) ||
                    acknowledgement != link_state.send_next)
                {
                    return;
                }
                link_state.receive_next = sequence + 1;
                link_state.send_unacknowledged = acknowledgement;
                bool peer_scales = false;
                for (std::size_t index = 0; index < options.size();)
                {
                    std::uint8_t const kind = options[index];
                    if (kind == 0)
                    {
                        break;
                    }
                    if (kind == 1)
                    {
                        ++index;
                        continue;
                    }
                    if (index + 1 >= options.size() || options[index + 1] < 2 ||
                        index + options[index + 1] > options.size())
                    {
                        break;
                    }
                    if (kind == 2 && options[index + 1] == 4)
                    {
                        link_state.peer_segment = get_be16(options.data() + index + 2);
                    }
                    else if (kind == 3 && options[index + 1] == 3)
                    {
                        link_state.peer_window_shift = std::min<unsigned>(options[index + 2], 14);
                        peer_scales = true;
                    }
                    index += options[index + 1];
                }
                // scaling is used only when both ends offered it
                link_state.own_window_shift = peer_scales ? offered_window_shift : 0;
                if (!peer_scales)
                {
                    link_state.peer_window_shift = 0;
                }
                link_state.peer_window = window; // never scaled in the phone's first segment
                link_state.connecting = false;
                link_state.changed.notify_all();
                answer = true;
            }
            else
            {
                if ((flags & flag_ack) != 0)
                {
                    if (before(link_state.send_unacknowledged, acknowledgement) &&
                        !before(link_state.send_next, acknowledgement))
                    {
                        link_state.send_unacknowledged = acknowledgement;
                    }
                    link_state.peer_window = static_cast<std::uint32_t>(window) << link_state.peer_window_shift;
                    link_state.changed.notify_all();
                }
                // A segment that does not begin with the byte this end expects next is answered
                // with what is expected.  The phone probes a connection that has been silent this
                // way (a "keep-alive": no data, one byte before the expected one) and resets the
                // connection when nobody answers - which is what happened to every connection
                // that stayed silent for a few minutes, the one of the screen stream first.
                if (sequence != link_state.receive_next)
                {
                    answer = true;
                }
                std::uint32_t end_of_data = sequence;
                if (!payload.empty())
                {
                    if (sequence == link_state.receive_next)
                    {
                        if (!link_state.closed_here)
                        {
                            link_state.received.insert(link_state.received.end(), payload.begin(), payload.end());
                        }
                        link_state.receive_next += static_cast<std::uint32_t>(payload.size());
                        link_state.changed.notify_all();
                    }
                    // (anything else cannot happen on this link; the answer below says what is expected)
                    end_of_data = sequence + static_cast<std::uint32_t>(payload.size());
                    answer = true;
                }
                if ((flags & flag_fin) != 0 && end_of_data == link_state.receive_next && !link_state.peer_finished)
                {
                    link_state.receive_next += 1;
                    link_state.peer_finished = true;
                    link_state.changed.notify_all();
                    answer = true;
                }
            }
            answer_sequence = link_state.send_next;
            answer_acknowledgement = link_state.receive_next;
            answer_window = link_state.window_field();
            link_state.announced_window = static_cast<std::uint32_t>(answer_window) << link_state.own_window_shift;
            if (link_state.closed_here && link_state.peer_finished)
            {
                connections.erase(found); // both directions are done
            }
        }
        if (answer)
        {
            if (status const sent =
                    send_segment(local_port, remote_port, answer_sequence, answer_acknowledgement, flag_ack,
                                 answer_window, {}, {});
                !sent)
            {
                mark_broken(sent.error().describe());
            }
        }
    }

    void device_tunnel::state::read_loop()
    {
        std::vector<std::uint8_t> payload;
        for (;;)
        {
            std::array<std::uint8_t, ipv6_header_bytes> header{};
            if (status const got = link->read_exact(header); !got)
            {
                mark_broken(got.error().describe());
                return;
            }
            payload.resize(get_be16(header.data() + 4));
            if (!payload.empty())
            {
                if (status const got = link->read_exact(payload); !got)
                {
                    mark_broken(got.error().describe());
                    return;
                }
            }
            if ((header[0] >> 4) != 6)
            {
                mark_broken("the tunnel carried something that is no IPv6 packet");
                return;
            }
            std::uint8_t const protocol = header[6];
            if (protocol == protocol_tcp)
            {
                handle_tcp(payload);
            }
            else if (protocol == protocol_udp && payload.size() >= udp_header_bytes)
            {
                std::uint16_t const port = get_be16(payload.data() + 2);
                std::scoped_lock lock{mutex};
                if (discard_ports.contains(port))
                {
                    ++discarded;
                }
            }
            // anything else (ICMPv6): not needed on a link with two fixed ends
        }
    }

    namespace
    {
        // the connection as the layers above see it
        class tunnel_stream final : public byte_stream
        {
        public:
            tunnel_stream(std::shared_ptr<device_tunnel::state> owner, std::shared_ptr<connection> shared)
                : tunnel{std::move(owner)},
                  link_state{std::move(shared)}
            {
            }

            ~tunnel_stream() override
            {
                finish(true);
            }

            outcome<std::size_t> read_some(std::span<std::uint8_t> buffer) override
            {
                if (buffer.empty())
                {
                    return std::size_t{0};
                }
                bool update_window = false;
                std::uint32_t sequence = 0;
                std::uint32_t acknowledgement = 0;
                std::uint16_t window = 0;
                std::size_t taken = 0;
                {
                    std::unique_lock lock{tunnel->mutex};
                    auto const something_to_report = [this]
                    {
                        return link_state->buffered() > 0 || link_state->peer_finished || link_state->reset ||
                               link_state->closed_here || tunnel->broken.load();
                    };
                    std::chrono::milliseconds const patience = link_state->read_patience;
                    if (patience == no_read_limit)
                    {
                        link_state->changed.wait(lock, something_to_report);
                    }
                    else if (!link_state->changed.wait_for(lock, patience, something_to_report))
                    {
                        return std::unexpected(make_failure(
                            "tunnel connection", 0,
                            std::format("the phone did not answer within {} ms", patience.count())));
                    }
                    if (link_state->buffered() == 0)
                    {
                        if (link_state->closed_here)
                        {
                            return std::unexpected(make_failure("tunnel connection", 0, "closed here"));
                        }
                        if (link_state->reset)
                        {
                            return std::unexpected(make_failure("tunnel connection", 0, "reset by the phone"));
                        }
                        if (link_state->peer_finished)
                        {
                            return std::unexpected(make_failure("tunnel connection", 0, "closed by the phone"));
                        }
                        return std::unexpected(make_failure("tunnel", 0, tunnel->break_reason));
                    }
                    taken = std::min(buffer.size(), link_state->buffered());
                    std::memcpy(buffer.data(), link_state->received.data() + link_state->received_offset, taken);
                    link_state->received_offset += taken;
                    if (link_state->received_offset == link_state->received.size())
                    {
                        link_state->received.clear();
                        link_state->received_offset = 0;
                    }
                    else if (link_state->received_offset > receive_capacity / 2)
                    {
                        link_state->received.erase(link_state->received.begin(),
                                                   link_state->received.begin() +
                                                       static_cast<std::ptrdiff_t>(link_state->received_offset));
                        link_state->received_offset = 0;
                    }
                    // the phone learns that there is room again when the
                    // window it was told had become small
                    std::uint32_t const room = static_cast<std::uint32_t>(link_state->window_field())
                                               << link_state->own_window_shift;
                    if (link_state->announced_window < 2 * link_state->peer_segment &&
                        room >= 2 * link_state->peer_segment)
                    {
                        update_window = true;
                        sequence = link_state->send_next;
                        acknowledgement = link_state->receive_next;
                        window = link_state->window_field();
                        link_state->announced_window = room;
                    }
                }
                if (update_window)
                {
                    // (a failure here shows as a broken tunnel at the next call)
                    static_cast<void>(tunnel->send_segment(link_state->local_port, link_state->remote_port, sequence,
                                                           acknowledgement, flag_ack, window, {}, {}));
                }
                return taken;
            }

            status write_all(std::span<std::uint8_t const> data) override
            {
                std::scoped_lock write_lock{link_state->write_mutex};
                std::size_t offset = 0;
                while (offset < data.size())
                {
                    std::uint32_t sequence = 0;
                    std::uint32_t acknowledgement = 0;
                    std::uint16_t window = 0;
                    std::size_t chunk = 0;
                    {
                        std::unique_lock lock{tunnel->mutex};
                        bool const ready = link_state->changed.wait_for(lock, send_patience, [this]
                        {
                            return link_state->reset || link_state->closed_here || link_state->finish_sent ||
                                   tunnel->broken.load() ||
                                   link_state->send_next - link_state->send_unacknowledged < link_state->peer_window;
                        });
                        if (link_state->closed_here || link_state->finish_sent)
                        {
                            return std::unexpected(make_failure("tunnel connection", 0, "closed here"));
                        }
                        if (link_state->reset)
                        {
                            return std::unexpected(make_failure("tunnel connection", 0, "reset by the phone"));
                        }
                        if (tunnel->broken.load())
                        {
                            return std::unexpected(make_failure("tunnel", 0, tunnel->break_reason));
                        }
                        if (!ready)
                        {
                            return std::unexpected(
                                make_failure("tunnel connection", 0, "the phone does not take any more data"));
                        }
                        std::uint32_t const in_flight = link_state->send_next - link_state->send_unacknowledged;
                        chunk = std::min<std::size_t>({data.size() - offset, link_state->peer_window - in_flight,
                                                       link_state->peer_segment,
                                                       tunnel->mtu - ipv6_header_bytes - tcp_header_bytes});
                        sequence = link_state->send_next;
                        link_state->send_next += static_cast<std::uint32_t>(chunk);
                        acknowledgement = link_state->receive_next;
                        window = link_state->window_field();
                    }
                    bool const last = offset + chunk == data.size();
                    if (status const sent = tunnel->send_segment(
                            link_state->local_port, link_state->remote_port, sequence, acknowledgement,
                            static_cast<std::uint8_t>(flag_ack | (last ? flag_psh : 0)), window, {},
                            data.subspan(offset, chunk));
                        !sent)
                    {
                        tunnel->mark_broken(sent.error().describe());
                        return sent;
                    }
                    offset += chunk;
                }
                return {};
            }

            void shut_down() override
            {
                finish(false);
            }

            void set_read_patience(std::chrono::milliseconds patience) override
            {
                std::scoped_lock lock{tunnel->mutex};
                link_state->read_patience = patience;
            }

        private:
            // This end is done: a waiting read or write returns, the phone gets
            // a FIN.  `gone`: nobody will read what still arrives.
            void finish(bool gone)
            {
                bool send_finish = false;
                std::uint32_t sequence = 0;
                std::uint32_t acknowledgement = 0;
                std::uint16_t window = 0;
                {
                    std::scoped_lock lock{tunnel->mutex};
                    link_state->closed_here = true;
                    link_state->changed.notify_all();
                    if (!link_state->connecting && !link_state->reset && !link_state->finish_sent &&
                        !tunnel->broken.load())
                    {
                        link_state->finish_sent = true;
                        send_finish = true;
                        sequence = link_state->send_next;
                        link_state->send_next += 1;
                        acknowledgement = link_state->receive_next;
                        window = link_state->window_field();
                    }
                    if (gone && (link_state->peer_finished || link_state->reset || link_state->connecting ||
                                 tunnel->broken.load()))
                    {
                        tunnel->connections.erase({link_state->local_port, link_state->remote_port});
                    }
                }
                if (send_finish)
                {
                    static_cast<void>(tunnel->send_segment(link_state->local_port, link_state->remote_port, sequence,
                                                           acknowledgement, flag_fin | flag_ack, window, {}, {}));
                }
            }

            std::shared_ptr<device_tunnel::state> tunnel;
            std::shared_ptr<connection> link_state;
        };
    }

    device_tunnel::device_tunnel(std::shared_ptr<state> created_state)
        : impl{std::move(created_state)}
    {
    }

    device_tunnel::~device_tunnel()
    {
        impl->mark_broken("the tunnel was closed here");
        impl->link->shut_down();
        if (impl->reader.joinable())
        {
            impl->reader.join();
        }
    }

    outcome<std::unique_ptr<device_tunnel>> device_tunnel::open(lockdown_session &session)
    {
        outcome<std::unique_ptr<byte_stream>> service = session.start_service(tunnel_service_name);
        if (!service)
        {
            return std::unexpected(service.error());
        }
        auto created = std::make_shared<state>();
        created->link = std::move(*service);

        std::string const request = std::format("{{\"type\":\"clientHandshakeRequest\",\"mtu\":{}}}", requested_mtu);
        std::vector<std::uint8_t> packet(handshake_magic.begin(), handshake_magic.end());
        packet.push_back(static_cast<std::uint8_t>(request.size() >> 8));
        packet.push_back(static_cast<std::uint8_t>(request.size()));
        packet.insert(packet.end(), request.begin(), request.end());
        if (status const sent = created->link->write_all(packet); !sent)
        {
            return std::unexpected(sent.error());
        }
        std::array<std::uint8_t, 10> header{};
        if (status const got = created->link->read_exact(header); !got)
        {
            return std::unexpected(got.error());
        }
        if (!std::ranges::equal(std::span{header}.first(handshake_magic.size()), handshake_magic))
        {
            return std::unexpected(make_failure("tunnel handshake", 0, "the phone answered with something else"));
        }
        std::string reply(get_be16(header.data() + 8), '\0');
        if (status const got =
                created->link->read_exact(std::span{reinterpret_cast<std::uint8_t *>(reply.data()), reply.size()});
            !got)
        {
            return std::unexpected(got.error());
        }
        // {"serverAddress":"..","serverRSDPort":n,"clientParameters":{"address":"..","mtu":n,..},..}
        std::optional<std::string> const phone_text = json_text(reply, "serverAddress");
        std::optional<std::string> const local_text = json_text(reply, "address");
        std::optional<std::uint32_t> const directory_port = json_number(reply, "serverRSDPort");
        if (!phone_text || !local_text || !directory_port || *directory_port == 0 || *directory_port > 65535)
        {
            return std::unexpected(make_failure("tunnel handshake", 0, "the phone's answer is incomplete"));
        }
        std::optional<address_bytes> const phone = parse_address(*phone_text);
        std::optional<address_bytes> const local = parse_address(*local_text);
        if (!phone || !local)
        {
            return std::unexpected(make_failure("tunnel handshake", 0, "the phone named an address that is none"));
        }
        created->phone = *phone;
        created->local = *local;
        created->phone_text = *phone_text;
        created->local_text = *local_text;
        created->directory_port = static_cast<std::uint16_t>(*directory_port);
        created->mtu = std::clamp<std::uint32_t>(json_number(reply, "mtu").value_or(1280), 1280, requested_mtu);
        // (ports of this end start at a different place each time)
        created->next_port = static_cast<std::uint16_t>(first_local_port + std::random_device{}() % 8192);

        // from here on the link carries packets whenever one of the two ends
        // has one: its reader waits for as long as it takes
        created->link->set_read_patience(no_read_limit);
        state *const running = created.get();
        created->reader = std::thread{[running] { running->read_loop(); }};
        return std::unique_ptr<device_tunnel>{new device_tunnel{std::move(created)}};
    }

    std::uint16_t device_tunnel::discovery_port() const
    {
        return impl->directory_port;
    }

    std::string const &device_tunnel::local_address() const
    {
        return impl->local_text;
    }

    std::string const &device_tunnel::phone_address() const
    {
        return impl->phone_text;
    }

    bool device_tunnel::alive() const
    {
        return !impl->broken.load();
    }

    std::uint16_t device_tunnel::discard_port()
    {
        std::scoped_lock lock{impl->mutex};
        std::uint16_t const port = impl->next_port++;
        impl->discard_ports.insert(port);
        return port;
    }

    std::uint64_t device_tunnel::discarded_datagrams() const
    {
        return impl->discarded.load();
    }

    outcome<std::unique_ptr<byte_stream>> device_tunnel::connect(std::uint16_t port)
    {
        auto link_state = std::make_shared<connection>();
        std::uint32_t initial_sequence = 0;
        {
            std::scoped_lock lock{impl->mutex};
            if (impl->broken.load())
            {
                return std::unexpected(make_failure("tunnel", 0, impl->break_reason));
            }
            link_state->local_port = impl->next_port++;
            if (impl->next_port < first_local_port)
            {
                impl->next_port = first_local_port; // wrapped
            }
            link_state->remote_port = port;
            initial_sequence = std::random_device{}();
            link_state->send_unacknowledged = initial_sequence;
            link_state->send_next = initial_sequence + 1; // the SYN takes one number
            impl->connections[{link_state->local_port, port}] = link_state;
        }
        // options: largest segment this end takes, no-op, window scale
        std::array<std::uint8_t, 8> options{2, 4, 0, 0, 1, 3, 3, static_cast<std::uint8_t>(offered_window_shift)};
        put_be16(options.data() + 2, static_cast<std::uint16_t>(impl->mtu - ipv6_header_bytes - tcp_header_bytes));
        auto stream = std::make_unique<tunnel_stream>(impl, link_state);
        if (status const sent =
                impl->send_segment(link_state->local_port, port, initial_sequence, 0, flag_syn, 0xFFFF, options, {});
            !sent)
        {
            impl->mark_broken(sent.error().describe());
            return std::unexpected(sent.error());
        }
        std::unique_lock lock{impl->mutex};
        bool const answered = link_state->changed.wait_for(lock, connect_patience, [this, &link_state]
        {
            return !link_state->connecting || impl->broken.load();
        });
        if (impl->broken.load())
        {
            return std::unexpected(make_failure("tunnel", 0, impl->break_reason));
        }
        if (!answered)
        {
            return std::unexpected(make_failure("tunnel connection", port, "the phone did not answer on that port"));
        }
        if (link_state->reset)
        {
            return std::unexpected(make_failure("tunnel connection", port, "the phone refused the connection"));
        }
        lock.unlock();
        return std::unique_ptr<byte_stream>{std::move(stream)};
    }
}

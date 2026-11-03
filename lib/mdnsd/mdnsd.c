/**
 *  Copyright (C) 2026  kgbook
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include "../compat.h"
#include "mdnsd.h"

#define MDNS_ADDR4 "224.0.0.251"
#define MDNS_ADDR6 "ff02::fb"
#define MDNS_PORT 5353
#define MDNS_MAX_PACKET 1500
#define MDNS_COMBINED_PACKET_MAX 1200
#define MDNS_TTL_HOST 120

#define DNS_TYPE_A 1
#define DNS_TYPE_AAAA 28
#define DNS_TYPE_PTR 12
#define DNS_TYPE_TXT 16
#define DNS_TYPE_SRV 33
#define DNS_TYPE_ANY 255
#define DNS_CLASS_IN 1
#define DNS_FLAG_RESPONSE 0x8000
#define DNS_FLAG_AUTHORITATIVE 0x0400
#define DNS_CACHE_FLUSH 0x8000
#define DNS_UNICAST_RESPONSE 0x8000

typedef struct {
    unsigned char bytes[MDNS_MAX_PACKET];
    size_t length;
} mdns_packet_t;

typedef enum {
    MDNS_FAMILY_IPV4,
    MDNS_FAMILY_IPV6
} mdns_family_t;

struct mdnsd_s {
    char host_name[MDNSD_MAX_NAME];
    mdnsd_service_t airplay;
    mdnsd_service_t raop;

    uint32_t ipv4_addr;
    unsigned char ipv6_addr[16];
    unsigned int ipv6_scope_id;
    int sock_fd4;
    int sock_fd6;
    int running;
    /* an AirPlay session is open: keep advertising on its link */
    int session_active;
    /* 0 = "Wi-Fi only": never advertise on the iPhone USB link */
    int usb_link_allowed;
    /* the user changed the link preference: re-pick now, even mid-session */
    int link_choice_changed;
    thread_handle_t thread;
    mutex_handle_t mutex;
};

/* how often the advertised link is re-evaluated (USB plugged in / removed) */
#define MDNS_LINK_CHECK_MS 2000
/* RFC 6762 section 8.3: repeat an announcement once, one second later */
#define MDNS_REANNOUNCE_MS 1000

static uint64_t mdns_now_ms(void)
{
    return (uint64_t) GetTickCount64();
}

static int mdns_put_u8(mdns_packet_t *packet, unsigned int value)
{
    if (packet->length + 1 > sizeof(packet->bytes)) {
        return -1;
    }
    packet->bytes[packet->length++] = (unsigned char) value;
    return 0;
}

static int mdns_put_u16(mdns_packet_t *packet, unsigned int value)
{
    if (packet->length + 2 > sizeof(packet->bytes)) {
        return -1;
    }
    packet->bytes[packet->length++] = (unsigned char) ((value >> 8) & 0xff);
    packet->bytes[packet->length++] = (unsigned char) (value & 0xff);
    return 0;
}

static int mdns_put_u32(mdns_packet_t *packet, uint32_t value)
{
    if (packet->length + 4 > sizeof(packet->bytes)) {
        return -1;
    }
    packet->bytes[packet->length++] = (unsigned char) ((value >> 24) & 0xff);
    packet->bytes[packet->length++] = (unsigned char) ((value >> 16) & 0xff);
    packet->bytes[packet->length++] = (unsigned char) ((value >> 8) & 0xff);
    packet->bytes[packet->length++] = (unsigned char) (value & 0xff);
    return 0;
}

static int mdns_put_bytes(mdns_packet_t *packet, const void *bytes, size_t length)
{
    if (packet->length + length > sizeof(packet->bytes)) {
        return -1;
    }
    memcpy(packet->bytes + packet->length, bytes, length);
    packet->length += length;
    return 0;
}

static int mdns_put_name(mdns_packet_t *packet, const char *name)
{
    const char *label = name;

    while (label && *label) {
        const char *dot = strchr(label, '.');
        size_t length = dot ? (size_t) (dot - label) : strlen(label);

        if (length > 63 || mdns_put_u8(packet, (unsigned int) length)) {
            return -1;
        }
        if (mdns_put_bytes(packet, label, length)) {
            return -1;
        }
        if (!dot) {
            break;
        }
        label = dot + 1;
    }

    return mdns_put_u8(packet, 0);
}

static int mdns_begin_rr(mdns_packet_t *packet, const char *name, unsigned int type,
                         unsigned int cls, uint32_t ttl, size_t *rdlength_pos)
{
    if (mdns_put_name(packet, name) ||
        mdns_put_u16(packet, type) ||
        mdns_put_u16(packet, cls) ||
        mdns_put_u32(packet, ttl)) {
        return -1;
    }
    *rdlength_pos = packet->length;
    return mdns_put_u16(packet, 0);
}

static void mdns_end_rr(mdns_packet_t *packet, size_t rdlength_pos)
{
    size_t rdlength = packet->length - rdlength_pos - 2;
    packet->bytes[rdlength_pos] = (unsigned char) ((rdlength >> 8) & 0xff);
    packet->bytes[rdlength_pos + 1] = (unsigned char) (rdlength & 0xff);
}

static int mdns_add_ptr(mdns_packet_t *packet, const char *name, const char *ptr,
                        uint32_t ttl)
{
    size_t rdlength_pos;
    if (mdns_begin_rr(packet, name, DNS_TYPE_PTR, DNS_CLASS_IN, ttl, &rdlength_pos) ||
        mdns_put_name(packet, ptr)) {
        return -1;
    }
    mdns_end_rr(packet, rdlength_pos);
    return 0;
}

static int mdns_add_srv(mdns_packet_t *packet, const char *name, unsigned short port,
                        const char *target, uint32_t ttl)
{
    size_t rdlength_pos;
    if (mdns_begin_rr(packet, name, DNS_TYPE_SRV, DNS_CLASS_IN | DNS_CACHE_FLUSH,
                      ttl, &rdlength_pos) ||
        mdns_put_u16(packet, 0) ||
        mdns_put_u16(packet, 0) ||
        mdns_put_u16(packet, port) ||
        mdns_put_name(packet, target)) {
        return -1;
    }
    mdns_end_rr(packet, rdlength_pos);
    return 0;
}

static int mdns_add_txt(mdns_packet_t *packet, const char *name, const mdnsd_txt_t *txt,
                        uint32_t ttl)
{
    size_t rdlength_pos;
    if (mdns_begin_rr(packet, name, DNS_TYPE_TXT, DNS_CLASS_IN | DNS_CACHE_FLUSH,
                      ttl, &rdlength_pos) ||
        mdns_put_bytes(packet, txt->bytes, (size_t) txt->length)) {
        return -1;
    }
    mdns_end_rr(packet, rdlength_pos);
    return 0;
}

static int mdns_add_a(mdns_packet_t *packet, const char *name, uint32_t addr,
                      uint32_t ttl)
{
    size_t rdlength_pos;
    if (!addr) {
        return 0;
    }
    if (mdns_begin_rr(packet, name, DNS_TYPE_A, DNS_CLASS_IN | DNS_CACHE_FLUSH,
                      ttl, &rdlength_pos) ||
        mdns_put_bytes(packet, &addr, sizeof(addr))) {
        return -1;
    }
    mdns_end_rr(packet, rdlength_pos);
    return 0;
}

static int mdns_has_ipv6_addr(const unsigned char addr[16])
{
    static const unsigned char zero[16] = {0};
    return memcmp(addr, zero, sizeof(zero)) != 0;
}

static int mdns_add_aaaa(mdns_packet_t *packet, const char *name,
                         const unsigned char addr[16], uint32_t ttl)
{
    size_t rdlength_pos;

    if (!mdns_has_ipv6_addr(addr)) {
        return 0;
    }
    if (mdns_begin_rr(packet, name, DNS_TYPE_AAAA,
                      DNS_CLASS_IN | DNS_CACHE_FLUSH, ttl, &rdlength_pos) ||
        mdns_put_bytes(packet, addr, 16)) {
        return -1;
    }
    mdns_end_rr(packet, rdlength_pos);
    return 0;
}

static int mdns_add_host_records(mdnsd_t *mdnsd, mdns_packet_t *packet,
                                 uint32_t ttl, unsigned int *answers,
                                 mdns_family_t family)
{
    /*
     * Treat the IPv4 and IPv6 sockets as logical mDNS interfaces. This keeps
     * AirPlay clients from seeing cross-family host records in one response
     * and opening two TCP connections for the same service.
     */
    if (family == MDNS_FAMILY_IPV4) {
        if (mdns_add_a(packet, mdnsd->host_name, mdnsd->ipv4_addr, ttl)) {
            return -1;
        }
        if (mdnsd->ipv4_addr) {
            (*answers)++;
        }
    } else {
        if (mdns_add_aaaa(packet, mdnsd->host_name, mdnsd->ipv6_addr, ttl)) {
            return -1;
        }
        if (mdns_has_ipv6_addr(mdnsd->ipv6_addr)) {
            (*answers)++;
        }
    }

    return 0;
}

static uint16_t mdns_read_u16(const unsigned char *bytes)
{
    return (uint16_t) (((uint16_t) bytes[0] << 8) | bytes[1]);
}

static int mdns_read_name(const unsigned char *packet, size_t packet_len,
                          size_t *offset, char *name, size_t name_len)
{
    size_t pos = *offset;
    size_t out = 0;
    int jumped = 0;
    int jumps = 0;

    while (pos < packet_len) {
        unsigned int length = packet[pos++];

        if (length == 0) {
            if (!jumped) {
                *offset = pos;
            }
            if (out == 0) {
                if (name_len < 2) {
                    return -1;
                }
                name[out++] = '.';
            }
            name[out] = '\0';
            return 0;
        }

        if ((length & 0xc0) == 0xc0) {
            unsigned int ptr;
            if (pos >= packet_len || ++jumps > 16) {
                return -1;
            }
            ptr = ((length & 0x3f) << 8) | packet[pos++];
            if (ptr >= packet_len) {
                return -1;
            }
            if (!jumped) {
                *offset = pos;
            }
            pos = ptr;
            jumped = 1;
            continue;
        }

        if ((length & 0xc0) || pos + length > packet_len) {
            return -1;
        }
        if (out) {
            if (out + 1 >= name_len) {
                return -1;
            }
            name[out++] = '.';
        }
        if (out + length >= name_len) {
            return -1;
        }
        memcpy(name + out, packet + pos, length);
        out += length;
        pos += length;
    }

    return -1;
}

static int mdns_name_equals(const char *left, const char *right)
{
    while (*left && *right) {
        if (tolower((unsigned char) *left) != tolower((unsigned char) *right)) {
            return 0;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static int mdns_query_type_matches(uint16_t query_type, uint16_t record_type)
{
    return query_type == record_type || query_type == DNS_TYPE_ANY;
}

/* The iPhone's wired link (Personal Hotspot over the USB cable) always uses
 * 172.20.10.0/24 (addresses in network byte order). */
static int mdns_is_usb_link(uint32_t addr)
{
    return (ntohl(addr) >> 8) == 0xAC140Au;
}

static int mdns_is_link_local(uint32_t addr)
{
    return (ntohl(addr) >> 16) == 0xA9FEu;
}

/*
 * Picks the IPv4 address to advertise.  The iPhone USB link wins whenever it
 * is up, so AirPlay rides the cable (USB first, Wi-Fi second); otherwise the
 * internet-facing LAN adapter that Windows itself prefers (lowest interface
 * metric).  Only real LAN links (Ethernet, Wi-Fi) carry mDNS to the phone:
 * loopback, Hyper-V/WSL switches, VM adapters and tunnels (Tailscale, VPN)
 * are skipped.  With allow_usb_link == 0 ("Wi-Fi only") the USB link is never
 * chosen, so the cable can stay plugged in for charging.  *held_present
 * reports whether `held` (the address in use) is still configured on any
 * adapter.
 */
static uint32_t mdns_pick_ipv4(uint32_t held, int *held_present, int allow_usb_link)
{
    uint32_t addr = 0;
    *held_present = 0;
    ULONG const flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                        GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG buflen = 15000;
    PIP_ADAPTER_ADDRESSES pAddresses = NULL;
    DWORD dwRetVal = ERROR_BUFFER_OVERFLOW;

    /* the adapter list can grow between the two calls: retry a few times */
    for (int attempt = 0; attempt < 3 && dwRetVal == ERROR_BUFFER_OVERFLOW; attempt++) {
        free(pAddresses);
        pAddresses = (IP_ADAPTER_ADDRESSES *) malloc(buflen);
        if (!pAddresses) {
            return 0;
        }
        dwRetVal = GetAdaptersAddresses(AF_INET, flags, NULL, pAddresses, &buflen);
    }

    if (dwRetVal == NO_ERROR) {
        uint32_t usb_link = 0;
        uint32_t fallback = 0;
        ULONG fallback_metric = 0xFFFFFFFFu;
        for (PIP_ADAPTER_ADDRESSES pCurr = pAddresses; pCurr != NULL; pCurr = pCurr->Next) {
            if (pCurr->OperStatus != IfOperStatusUp) {
                continue;
            }
            int const lan_link =
                (pCurr->IfType == IF_TYPE_ETHERNET_CSMACD || pCurr->IfType == IF_TYPE_IEEE80211) &&
                (pCurr->Flags & IP_ADAPTER_NO_MULTICAST) == 0 &&
                wcsstr(pCurr->Description, L"Hyper-V") == NULL &&
                wcsstr(pCurr->Description, L"Virtual") == NULL &&
                wcsstr(pCurr->Description, L"WSL") == NULL &&
                wcsstr(pCurr->Description, L"vEthernet") == NULL;
            for (PIP_ADAPTER_UNICAST_ADDRESS ua = pCurr->FirstUnicastAddress; ua != NULL; ua = ua->Next) {
                if (ua->Address.lpSockaddr == NULL || ua->Address.lpSockaddr->sa_family != AF_INET) {
                    continue;
                }
                uint32_t const candidate = ((struct sockaddr_in *) ua->Address.lpSockaddr)->sin_addr.s_addr;
                if (held != 0 && candidate == held) {
                    *held_present = 1;
                }
                /* skip addresses still in duplicate-address detection */
                if (!lan_link || ua->DadState != IpDadStatePreferred) {
                    continue;
                }
                if (mdns_is_usb_link(candidate)) {
                    if (allow_usb_link && usb_link == 0) {
                        usb_link = candidate;
                    }
                } else if (pCurr->FirstGatewayAddress != NULL && !mdns_is_link_local(candidate) &&
                           pCurr->Ipv4Metric < fallback_metric) {
                    fallback = candidate;
                    fallback_metric = pCurr->Ipv4Metric;
                }
            }
        }
        addr = usb_link != 0 ? usb_link : fallback;
    }
    free(pAddresses);
    return addr;
}

static uint32_t mdns_get_default_ipv4(int allow_usb_link)
{
    int unused = 0;
    return mdns_pick_ipv4(0, &unused, allow_usb_link);
}

static int mdns_get_default_ipv6(unsigned char addr[16], unsigned int *scope_id)
{
    (void) addr;
    (void) scope_id;
    return 0;
}

static int mdns_open_socket4(uint32_t iface_addr)
{
    int fd;
    int one = 1;
    unsigned char ttl = 255;
    unsigned char loop = 1;
    struct sockaddr_in addr;
    struct ip_mreq mreq;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) {
        return -SOCKET_GET_ERROR();
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char *) &one, sizeof(one));
#endif
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, (const char *) &ttl, sizeof(ttl));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *) &loop, sizeof(loop));

    if (iface_addr) {
        struct in_addr iface;
        iface.s_addr = iface_addr;
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, (const char *) &iface, sizeof(iface));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MDNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
        int error = SOCKET_GET_ERROR();
        CLOSESOCKET(fd);
        return -error;
    }

    memset(&mreq, 0, sizeof(mreq));
    inet_pton(AF_INET, MDNS_ADDR4, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = iface_addr ? iface_addr : htonl(INADDR_ANY);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   (const char *) &mreq, sizeof(mreq)) == -1) {
        int error = SOCKET_GET_ERROR();
        if (iface_addr) {
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                           (const char *) &mreq, sizeof(mreq)) == 0) {
                return fd;
            }
        }
        CLOSESOCKET(fd);
        return -error;
    }

    return fd;
}

static int mdns_open_socket6(unsigned int scope_id)
{
    int fd;
    int one = 1;
    int hops = 255;
    unsigned int loop = 1;
    struct sockaddr_in6 addr;
    struct ipv6_mreq mreq;

    if (!scope_id) {
        return -1;
    }

    fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd == -1) {
        return -SOCKET_GET_ERROR();
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char *) &one, sizeof(one));
#endif
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char *) &one, sizeof(one));
    setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, (const char *) &hops, sizeof(hops));
    setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, (const char *) &loop, sizeof(loop));
    setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, (const char *) &scope_id, sizeof(scope_id));

    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(MDNS_PORT);
    addr.sin6_addr = in6addr_any;

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
        int error = SOCKET_GET_ERROR();
        CLOSESOCKET(fd);
        return -error;
    }

    memset(&mreq, 0, sizeof(mreq));
    inet_pton(AF_INET6, MDNS_ADDR6, &mreq.ipv6mr_multiaddr);
    mreq.ipv6mr_interface = scope_id;
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP,
                   (const char *) &mreq, sizeof(mreq)) == -1) {
        int error = SOCKET_GET_ERROR();
        CLOSESOCKET(fd);
        return -error;
    }

    return fd;
}

static int mdns_begin_response(mdns_packet_t *packet, size_t *count_pos)
{
    memset(packet, 0, sizeof(*packet));
    if (mdns_put_u16(packet, 0) ||
        mdns_put_u16(packet, DNS_FLAG_RESPONSE | DNS_FLAG_AUTHORITATIVE) ||
        mdns_put_u16(packet, 0)) {
        return -1;
    }
    *count_pos = packet->length;
    return mdns_put_u16(packet, 0) ||
           mdns_put_u16(packet, 0) ||
           mdns_put_u16(packet, 0);
}

static void mdns_finish_response(mdns_packet_t *packet, size_t count_pos,
                                 unsigned int answers)
{
    packet->bytes[count_pos] = (unsigned char) ((answers >> 8) & 0xff);
    packet->bytes[count_pos + 1] = (unsigned char) (answers & 0xff);
}

static int mdns_add_service_records(mdns_packet_t *packet,
                                    const mdnsd_service_t *service,
                                    const char *host_name, uint32_t ttl)
{
    if (!service->registered) {
        return 0;
    }

    return mdns_add_ptr(packet, "_services._dns-sd._udp.local", service->type, ttl) ||
           mdns_add_ptr(packet, service->type, service->name, ttl) ||
           mdns_add_srv(packet, service->name, service->port, host_name,
                        ttl ? MDNS_TTL_HOST : 0) ||
           mdns_add_txt(packet, service->name, &service->txt, ttl);
}

static int mdns_build_response(mdnsd_t *mdnsd, mdns_packet_t *packet,
                               const mdnsd_service_t *service,
                               int include_host, uint32_t ttl,
                               mdns_family_t family)
{
    size_t count_pos;
    unsigned int answers = 0;
    size_t before;

    if (mdns_begin_response(packet, &count_pos)) {
        return -1;
    }

    if (service && service->registered) {
        before = packet->length;
        if (mdns_add_service_records(packet, service, mdnsd->host_name, ttl)) {
            return -1;
        }
        if (packet->length != before) {
            answers += 4;
        }
    }

    if (include_host &&
        mdns_add_host_records(mdnsd, packet, ttl ? MDNS_TTL_HOST : 0,
                              &answers, family)) {
        return -1;
    }

    mdns_finish_response(packet, count_pos, answers);
    return answers ? 0 : -1;
}

static int mdns_build_combined_response(mdnsd_t *mdnsd, mdns_packet_t *packet,
                                        int include_airplay, int include_raop,
                                        int include_host, uint32_t ttl,
                                        mdns_family_t family)
{
    size_t count_pos;
    unsigned int answers = 0;
    size_t before;

    if (mdns_begin_response(packet, &count_pos)) {
        return -1;
    }

    if (include_airplay && mdnsd->airplay.registered) {
        before = packet->length;
        if (mdns_add_service_records(packet, &mdnsd->airplay, mdnsd->host_name,
                                     ttl)) {
            return -1;
        }
        if (packet->length != before) {
            answers += 4;
        }
    }

    if (include_raop && mdnsd->raop.registered) {
        before = packet->length;
        if (mdns_add_service_records(packet, &mdnsd->raop, mdnsd->host_name,
                                     ttl)) {
            return -1;
        }
        if (packet->length != before) {
            answers += 4;
        }
    }

    if (include_host &&
        mdns_add_host_records(mdnsd, packet, ttl ? MDNS_TTL_HOST : 0,
                              &answers, family)) {
        return -1;
    }

    if (!answers || packet->length > MDNS_COMBINED_PACKET_MAX) {
        return -1;
    }

    mdns_finish_response(packet, count_pos, answers);
    return 0;
}

static void mdns_send_packet4(mdnsd_t *mdnsd, const mdns_packet_t *packet,
                              const struct sockaddr_in *to)
{
    struct sockaddr_in addr;

    if (mdnsd->sock_fd4 == -1 || !packet->length) {
        return;
    }

    if (to) {
        addr = *to;
    } else {
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(MDNS_PORT);
        inet_pton(AF_INET, MDNS_ADDR4, &addr.sin_addr);
    }

    sendto(mdnsd->sock_fd4, (const char *) packet->bytes, (int) packet->length, 0,
           (struct sockaddr *) &addr, sizeof(addr));
}

static void mdns_send_packet6(mdnsd_t *mdnsd, const mdns_packet_t *packet,
                              const struct sockaddr_in6 *to)
{
    struct sockaddr_in6 addr;

    if (mdnsd->sock_fd6 == -1 || !packet->length) {
        return;
    }

    if (to) {
        addr = *to;
    } else {
        memset(&addr, 0, sizeof(addr));
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(MDNS_PORT);
        addr.sin6_scope_id = mdnsd->ipv6_scope_id;
        inet_pton(AF_INET6, MDNS_ADDR6, &addr.sin6_addr);
    }

    sendto(mdnsd->sock_fd6, (const char *) packet->bytes, (int) packet->length, 0,
           (struct sockaddr *) &addr, sizeof(addr));
}

static void mdns_send_service_locked(mdnsd_t *mdnsd, const mdnsd_service_t *service,
                                     int include_host, uint32_t ttl,
                                     mdns_family_t family, const void *to)
{
    mdns_packet_t packet;

    if (service->registered &&
        !mdns_build_response(mdnsd, &packet, service, include_host, ttl,
                             family)) {
        if (family == MDNS_FAMILY_IPV4) {
            mdns_send_packet4(mdnsd, &packet, (const struct sockaddr_in *) to);
        } else {
            mdns_send_packet6(mdnsd, &packet, (const struct sockaddr_in6 *) to);
        }
    }
}

static void mdns_send_response_locked(mdnsd_t *mdnsd, int include_airplay,
                                      int include_raop, int include_host,
                                      uint32_t ttl, mdns_family_t family,
                                      const void *to)
{
    mdns_packet_t packet;

    if (include_airplay && include_raop &&
        mdnsd->airplay.registered && mdnsd->raop.registered &&
        !mdns_build_combined_response(mdnsd, &packet, include_airplay,
                                      include_raop, include_host, ttl,
                                      family)) {
        if (family == MDNS_FAMILY_IPV4) {
            mdns_send_packet4(mdnsd, &packet, (const struct sockaddr_in *) to);
        } else {
            mdns_send_packet6(mdnsd, &packet, (const struct sockaddr_in6 *) to);
        }
        return;
    }

    if (include_airplay) {
        mdns_send_service_locked(mdnsd, &mdnsd->airplay, include_host, ttl,
                                 family, to);
    }
    if (include_raop) {
        mdns_send_service_locked(mdnsd, &mdnsd->raop, include_host, ttl,
                                 family, to);
    }
    if (include_host && !include_airplay && !include_raop &&
        !mdns_build_response(mdnsd, &packet, NULL, 1, ttl, family)) {
        if (family == MDNS_FAMILY_IPV4) {
            mdns_send_packet4(mdnsd, &packet, (const struct sockaddr_in *) to);
        } else {
            mdns_send_packet6(mdnsd, &packet, (const struct sockaddr_in6 *) to);
        }
    }
}

static void mdns_announce_locked(mdnsd_t *mdnsd, uint32_t ttl)
{
    mdns_send_response_locked(mdnsd, 1, 1, 1, ttl, MDNS_FAMILY_IPV4, NULL);
    mdns_send_response_locked(mdnsd, 1, 1, 1, ttl, MDNS_FAMILY_IPV6, NULL);
}

static int mdns_question_matches_service(const mdnsd_t *mdnsd, const char *name,
                                         uint16_t type, int *airplay, int *raop,
                                         int *host)
{
    if (mdns_name_equals(name, "_services._dns-sd._udp.local") &&
        mdns_query_type_matches(type, DNS_TYPE_PTR)) {
        if (mdnsd->airplay.registered) {
            *airplay = 1;
        }
        if (mdnsd->raop.registered) {
            *raop = 1;
        }
        return *airplay || *raop;
    }

    if (mdnsd->airplay.registered &&
        ((mdns_name_equals(name, mdnsd->airplay.type) &&
          mdns_query_type_matches(type, DNS_TYPE_PTR)) ||
         (mdns_name_equals(name, mdnsd->airplay.name) &&
          (mdns_query_type_matches(type, DNS_TYPE_TXT) ||
           mdns_query_type_matches(type, DNS_TYPE_SRV))))) {
        *airplay = 1;
        *host = 1;
        return 1;
    }

    if (mdnsd->raop.registered &&
        ((mdns_name_equals(name, mdnsd->raop.type) &&
          mdns_query_type_matches(type, DNS_TYPE_PTR)) ||
         (mdns_name_equals(name, mdnsd->raop.name) &&
          (mdns_query_type_matches(type, DNS_TYPE_TXT) ||
           mdns_query_type_matches(type, DNS_TYPE_SRV))))) {
        *raop = 1;
        *host = 1;
        return 1;
    }

    if (mdns_name_equals(name, mdnsd->host_name) &&
        (mdns_query_type_matches(type, DNS_TYPE_A) ||
         mdns_query_type_matches(type, DNS_TYPE_AAAA))) {
        *host = 1;
        return 1;
    }

    return 0;
}

static void mdns_handle_query(mdnsd_t *mdnsd, const unsigned char *bytes,
                              size_t length, mdns_family_t family,
                              const void *from, int from_port)
{
    uint16_t flags;
    uint16_t qdcount;
    size_t offset = 12;
    int include_airplay = 0;
    int include_raop = 0;
    int include_host = 0;
    int unicast_reply = 0;

    if (length < 12) {
        return;
    }

    flags = mdns_read_u16(bytes + 2);
    if (flags & 0x8000) {
        return;
    }

    qdcount = mdns_read_u16(bytes + 4);

    MUTEX_LOCK(mdnsd->mutex);
    for (uint16_t i = 0; i < qdcount; i++) {
        char name[MDNSD_MAX_NAME];
        uint16_t type;
        uint16_t cls;

        if (mdns_read_name(bytes, length, &offset, name, sizeof(name)) ||
            offset + 4 > length) {
            break;
        }
        type = mdns_read_u16(bytes + offset);
        cls = mdns_read_u16(bytes + offset + 2);
        offset += 4;

        if (cls & DNS_UNICAST_RESPONSE) {
            unicast_reply = 1;
        }
        mdns_question_matches_service(mdnsd, name, type, &include_airplay,
                                      &include_raop, &include_host);
    }

    if (include_airplay || include_raop || include_host) {
        mdns_send_response_locked(mdnsd, include_airplay, include_raop,
                                  include_host, MDNSD_TTL_SERVICE, family,
                                  NULL);
        if (from && (unicast_reply || from_port != htons(MDNS_PORT))) {
            mdns_send_response_locked(mdnsd, include_airplay, include_raop,
                                      include_host, MDNSD_TTL_SERVICE, family,
                                      from);
        }
    }
    MUTEX_UNLOCK(mdnsd->mutex);
}

/* Joins the IPv4 mDNS group on the link of `iface_addr`.  Returns 1 when a
 * membership was newly established (the OS had dropped it, e.g. the link
 * went down and up or the PC resumed from sleep), 0 when it was already in
 * place or the link is unavailable. */
static int mdns_join_group4_locked(mdnsd_t *mdnsd, uint32_t iface_addr)
{
    struct ip_mreq mreq;

    if (mdnsd->sock_fd4 == -1 || iface_addr == 0) {
        return 0;
    }
    memset(&mreq, 0, sizeof(mreq));
    inet_pton(AF_INET, MDNS_ADDR4, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = iface_addr;
    return setsockopt(mdnsd->sock_fd4, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                      (const char *) &mreq, sizeof(mreq)) == 0;
}

static void mdns_leave_group4_locked(mdnsd_t *mdnsd, uint32_t iface_addr)
{
    struct ip_mreq mreq;

    if (mdnsd->sock_fd4 == -1 || iface_addr == 0) {
        return;
    }
    memset(&mreq, 0, sizeof(mreq));
    inet_pton(AF_INET, MDNS_ADDR4, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = iface_addr;
    /* fails harmlessly when that link has already vanished */
    setsockopt(mdnsd->sock_fd4, IPPROTO_IP, IP_DROP_MEMBERSHIP,
               (const char *) &mreq, sizeof(mreq));
}

/*
 * Moves the IPv4 advertisement to another link (the iPhone USB link was
 * plugged in or removed while running): a goodbye on the old link makes the
 * phone forget the address it learned there (so its next connection uses the
 * new link), the multicast group follows so the phone's queries on the new
 * link are answered, then the services are announced there.
 */
static void mdns_move_ipv4_locked(mdnsd_t *mdnsd, uint32_t new_addr)
{
    struct in_addr iface;
    uint32_t const old_addr = mdnsd->ipv4_addr;

    if (old_addr != 0) {
        mdns_send_response_locked(mdnsd, 1, 1, 1, 0, MDNS_FAMILY_IPV4, NULL);
        mdns_leave_group4_locked(mdnsd, old_addr);
    }
    mdnsd->ipv4_addr = new_addr;
    iface.s_addr = new_addr;
    setsockopt(mdnsd->sock_fd4, IPPROTO_IP, IP_MULTICAST_IF, (const char *) &iface, sizeof(iface));
    mdns_join_group4_locked(mdnsd, new_addr);
    mdns_send_response_locked(mdnsd, 1, 1, 1, MDNSD_TTL_SERVICE, MDNS_FAMILY_IPV4, NULL);
}

/*
 * Live-follows the preferred link (USB first, Wi-Fi second): when the iPhone
 * USB link appears or disappears while running, the advertisement moves to
 * it without a restart.  An open AirPlay session pins the advertisement to
 * its link until the session ends (moving mid-session could make the phone
 * drop it), unless that link is gone or the user just changed the link
 * preference (the host then ends the session itself).  Also re-asserts the
 * multicast membership, which the OS drops when a link goes down and up.
 * Returns 1 when the services were (re)announced.
 */
static int mdns_follow_link(mdnsd_t *mdnsd)
{
    uint32_t held;
    uint32_t preferred;
    int held_present = 0;
    int announced = 0;
    int allow_usb_link;
    int user_choice;

    MUTEX_LOCK(mdnsd->mutex);
    held = mdnsd->ipv4_addr;
    allow_usb_link = mdnsd->usb_link_allowed;
    user_choice = mdnsd->link_choice_changed;
    mdnsd->link_choice_changed = 0;
    MUTEX_UNLOCK(mdnsd->mutex);

    preferred = mdns_pick_ipv4(held, &held_present, allow_usb_link);

    MUTEX_LOCK(mdnsd->mutex);
    if (mdnsd->sock_fd4 != -1 && mdnsd->ipv4_addr == held) {
        if (preferred != 0 && preferred != held &&
            (user_choice || !(mdnsd->session_active && held_present))) {
            mdns_move_ipv4_locked(mdnsd, preferred);
            announced = 1;
        } else if (held_present && mdns_join_group4_locked(mdnsd, held)) {
            mdns_send_response_locked(mdnsd, 1, 1, 1, MDNSD_TTL_SERVICE, MDNS_FAMILY_IPV4, NULL);
            announced = 1;
        }
    }
    MUTEX_UNLOCK(mdnsd->mutex);
    return announced;
}

static THREAD_RETVAL mdns_thread(void *arg)
{
    mdnsd_t *mdnsd = arg;
    uint64_t next_link_check = mdns_now_ms() + MDNS_LINK_CHECK_MS;
    uint64_t reannounce_at = 0;

    for (;;) {
        fd_set rfds;
        struct timeval tv;
        int fd4;
        int fd6;
        int max_fd = -1;
        int running;
        int check_now;
        int ret;

        MUTEX_LOCK(mdnsd->mutex);
        fd4 = mdnsd->sock_fd4;
        fd6 = mdnsd->sock_fd6;
        running = mdnsd->running;
        check_now = mdnsd->link_choice_changed;
        MUTEX_UNLOCK(mdnsd->mutex);

        if (!running || (fd4 == -1 && fd6 == -1)) {
            break;
        }

        FD_ZERO(&rfds);
        if (fd4 != -1) {
            FD_SET(fd4, &rfds);
            max_fd = fd4;
        }
        if (fd6 != -1) {
            FD_SET(fd6, &rfds);
            if (fd6 > max_fd) {
                max_fd = fd6;
            }
        }
        tv.tv_sec = 0;
        tv.tv_usec = 250000;

        ret = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret > 0 && fd4 != -1 && FD_ISSET(fd4, &rfds)) {
            unsigned char buffer[MDNS_MAX_PACKET];
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            int received = (int) recvfrom(fd4, (char *) buffer, sizeof(buffer), 0,
                                          (struct sockaddr *) &from, &from_len);
            if (received > 0) {
                mdns_handle_query(mdnsd, buffer, (size_t) received,
                                  MDNS_FAMILY_IPV4, &from, from.sin_port);
            }
        }
        if (ret > 0 && fd6 != -1 && FD_ISSET(fd6, &rfds)) {
            unsigned char buffer[MDNS_MAX_PACKET];
            struct sockaddr_in6 from;
            socklen_t from_len = sizeof(from);
            int received = (int) recvfrom(fd6, (char *) buffer, sizeof(buffer), 0,
                                          (struct sockaddr *) &from, &from_len);
            if (received > 0) {
                mdns_handle_query(mdnsd, buffer, (size_t) received,
                                  MDNS_FAMILY_IPV6, &from, from.sin6_port);
            }
        }

        /* time-based (incoming packets wake select early on busy networks) */
        uint64_t const now = mdns_now_ms();
        if (reannounce_at != 0 && now >= reannounce_at) {
            reannounce_at = 0;
            MUTEX_LOCK(mdnsd->mutex);
            mdns_send_response_locked(mdnsd, 1, 1, 1, MDNSD_TTL_SERVICE, MDNS_FAMILY_IPV4, NULL);
            MUTEX_UNLOCK(mdnsd->mutex);
        }
        if (check_now || now >= next_link_check) {
            next_link_check = now + MDNS_LINK_CHECK_MS;
            if (mdns_follow_link(mdnsd)) {
                reannounce_at = now + MDNS_REANNOUNCE_MS;
            }
        }
    }

    return NULL;
}

int mdnsd_txt_add(mdnsd_txt_t *txt, const char *key, const char *value)
{
    char item[256];
    int length = snprintf(item, sizeof(item), "%s=%s", key, value ? value : "");

    if (length < 0 || length > 255 ||
        txt->length + length + 1 > (int) sizeof(txt->bytes)) {
        return -1;
    }

    txt->bytes[txt->length++] = (unsigned char) length;
    memcpy(txt->bytes + txt->length, item, (size_t) length);
    txt->length += length;
    return 0;
}

mdnsd_t *mdnsd_init(const char *host_name)
{
    mdnsd_t *mdnsd = calloc(1, sizeof(*mdnsd));
    if (!mdnsd) {
        return NULL;
    }

    snprintf(mdnsd->host_name, sizeof(mdnsd->host_name), "%s",
             host_name && *host_name ? host_name : "UxPlay.local");
    mdnsd->sock_fd4 = -1;
    mdnsd->sock_fd6 = -1;
    mdnsd->usb_link_allowed = 1;
    MUTEX_CREATE(mdnsd->mutex);
    return mdnsd;
}

void mdnsd_destroy(mdnsd_t *mdnsd)
{
    if (!mdnsd) {
        return;
    }

    mdnsd_stop(mdnsd);
    MUTEX_DESTROY(mdnsd->mutex);
    free(mdnsd);
}

int mdnsd_start(mdnsd_t *mdnsd)
{
    if (!mdnsd) {
        return -1;
    }

    MUTEX_LOCK(mdnsd->mutex);
    if (mdnsd->running) {
        MUTEX_UNLOCK(mdnsd->mutex);
        return 0;
    }

    int error4 = 0;
    int error6 = 0;

    mdnsd->ipv4_addr = mdns_get_default_ipv4(mdnsd->usb_link_allowed);
    mdns_get_default_ipv6(mdnsd->ipv6_addr, &mdnsd->ipv6_scope_id);

    mdnsd->sock_fd4 = mdns_open_socket4(mdnsd->ipv4_addr);
    if (mdnsd->sock_fd4 < 0) {
        error4 = mdnsd->sock_fd4;
        mdnsd->sock_fd4 = -1;
    }

    mdnsd->sock_fd6 = mdns_open_socket6(mdnsd->ipv6_scope_id);
    if (mdnsd->sock_fd6 < 0) {
        error6 = mdnsd->sock_fd6;
        mdnsd->sock_fd6 = -1;
    }

    if (mdnsd->sock_fd4 == -1 && mdnsd->sock_fd6 == -1) {
        MUTEX_UNLOCK(mdnsd->mutex);
        return error4 ? error4 : error6;
    }

    mdnsd->running = 1;
    THREAD_CREATE(mdnsd->thread, mdns_thread, mdnsd);
    if (!mdnsd->thread) {
        if (mdnsd->sock_fd4 != -1) {
            CLOSESOCKET(mdnsd->sock_fd4);
            mdnsd->sock_fd4 = -1;
        }
        if (mdnsd->sock_fd6 != -1) {
            CLOSESOCKET(mdnsd->sock_fd6);
            mdnsd->sock_fd6 = -1;
        }
        mdnsd->running = 0;
        MUTEX_UNLOCK(mdnsd->mutex);
        return -1;
    }

    MUTEX_UNLOCK(mdnsd->mutex);
    return 0;
}

void mdnsd_stop(mdnsd_t *mdnsd)
{
    int fd4 = -1;
    int fd6 = -1;
    thread_handle_t thread = 0;

    if (!mdnsd) {
        return;
    }

    MUTEX_LOCK(mdnsd->mutex);
    if (mdnsd->running) {
        mdnsd->running = 0;
        fd4 = mdnsd->sock_fd4;
        fd6 = mdnsd->sock_fd6;
        thread = mdnsd->thread;
        mdnsd->sock_fd4 = -1;
        mdnsd->sock_fd6 = -1;
        mdnsd->thread = 0;
    }
    MUTEX_UNLOCK(mdnsd->mutex);

    if (fd4 != -1) {
        CLOSESOCKET(fd4);
    }
    if (fd6 != -1) {
        CLOSESOCKET(fd6);
    }
    if (thread) {
        THREAD_JOIN(thread);
    }
}

void mdnsd_set_services(mdnsd_t *mdnsd, const mdnsd_service_t *airplay,
                        const mdnsd_service_t *raop)
{
    if (!mdnsd) {
        return;
    }

    MUTEX_LOCK(mdnsd->mutex);
    if (airplay) {
        mdnsd->airplay = *airplay;
    } else {
        memset(&mdnsd->airplay, 0, sizeof(mdnsd->airplay));
    }
    if (raop) {
        mdnsd->raop = *raop;
    } else {
        memset(&mdnsd->raop, 0, sizeof(mdnsd->raop));
    }
    MUTEX_UNLOCK(mdnsd->mutex);
}

void mdnsd_announce(mdnsd_t *mdnsd, uint32_t ttl)
{
    if (!mdnsd) {
        return;
    }

    MUTEX_LOCK(mdnsd->mutex);
    mdns_announce_locked(mdnsd, ttl);
    MUTEX_UNLOCK(mdnsd->mutex);
}

void mdnsd_set_usb_link_allowed(mdnsd_t *mdnsd, int allowed)
{
    if (!mdnsd) {
        return;
    }

    MUTEX_LOCK(mdnsd->mutex);
    allowed = allowed ? 1 : 0;
    if (mdnsd->usb_link_allowed != allowed) {
        mdnsd->usb_link_allowed = allowed;
        /* picked up by mdns_thread within one select timeout (250 ms) */
        mdnsd->link_choice_changed = 1;
    }
    MUTEX_UNLOCK(mdnsd->mutex);
}

void mdnsd_set_session_active(mdnsd_t *mdnsd, int active)
{
    if (!mdnsd) {
        return;
    }

    MUTEX_LOCK(mdnsd->mutex);
    mdnsd->session_active = active ? 1 : 0;
    MUTEX_UNLOCK(mdnsd->mutex);
}

void mdnsd_goodbye(mdnsd_t *mdnsd, const mdnsd_service_t *service)
{
    if (!mdnsd || !service) {
        return;
    }

    MUTEX_LOCK(mdnsd->mutex);
    mdns_send_service_locked(mdnsd, service, 0, 0, MDNS_FAMILY_IPV4, NULL);
    mdns_send_service_locked(mdnsd, service, 0, 0, MDNS_FAMILY_IPV6, NULL);
    MUTEX_UNLOCK(mdnsd->mutex);
}

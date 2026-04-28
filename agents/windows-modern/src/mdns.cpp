//   Copyright 2026 William Isted and contributors
//
//   Licensed under the Apache License, Version 2.0 (the "License");
//   you may not use this file except in compliance with the License.
//   You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.

#include "mdns.hpp"

#include "log.hpp"

#include <atomic>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#pragma comment(lib, "iphlpapi.lib")

namespace remote_hands::mdns {

namespace {

constexpr const char* kMulticastAddr = "224.0.0.251";
constexpr std::uint16_t kMulticastPort = 5353;
constexpr std::string_view kServiceName = "_remote-hands";

// DNS resource-record types we emit.
constexpr std::uint16_t kRrA   = 1;
constexpr std::uint16_t kRrPtr = 12;
constexpr std::uint16_t kRrTxt = 16;
constexpr std::uint16_t kRrSrv = 33;
constexpr std::uint16_t kClassIn = 1;
constexpr std::uint32_t kTtlSeconds = 120;

// ---------------------------------------------------------------------------
// Wire-format helpers

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(v & 0xff));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xff));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((v >>  8) & 0xff));
    out.push_back(static_cast<std::uint8_t>( v        & 0xff));
}

// Encodes a DNS name as <len><label>...<0>. No compression.
void encode_name(std::vector<std::uint8_t>& out,
                 std::initializer_list<std::string_view> labels) {
    for (auto label : labels) {
        out.push_back(static_cast<std::uint8_t>(label.size()));
        for (char c : label) out.push_back(static_cast<std::uint8_t>(c));
    }
    out.push_back(0);
}

// ---------------------------------------------------------------------------
// Local IPv4 / hostname discovery

std::uint32_t find_local_ipv4_host_order() {
    ULONG bufsize = 16 * 1024;
    std::vector<BYTE> buf(bufsize);
    auto* head = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());

    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, head, &bufsize) !=
            NO_ERROR) {
        return 0;
    }

    for (auto* a = head; a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr) continue;
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            return ntohl(sin->sin_addr.s_addr);
        }
    }
    return 0;
}

std::string lower_ascii(const std::string& s) {
    std::string out(s);
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return out;
}

std::string get_hostname_lc() {
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf)) != 0) return "agent";
    return lower_ascii(buf);
}

// ---------------------------------------------------------------------------
// Response message builder

std::vector<std::uint8_t> build_response(const std::string& host,
                                         std::uint16_t tcp_port,
                                         const std::string& os_tag,
                                         std::uint32_t ipv4_host_order) {
    std::vector<std::uint8_t> msg;

    // Header: tx-id 0, flags 0x8400 (response, authoritative answer),
    // qdcount=0, ancount=4, nscount=0, arcount=0.
    put_u16(msg, 0);
    put_u16(msg, 0x8400);
    put_u16(msg, 0);
    put_u16(msg, 4);
    put_u16(msg, 0);
    put_u16(msg, 0);

    auto write_record = [&](std::initializer_list<std::string_view> name,
                            std::uint16_t type,
                            const std::vector<std::uint8_t>& rdata) {
        encode_name(msg, name);
        put_u16(msg, type);
        put_u16(msg, kClassIn);
        put_u32(msg, kTtlSeconds);
        put_u16(msg, static_cast<std::uint16_t>(rdata.size()));
        msg.insert(msg.end(), rdata.begin(), rdata.end());
    };

    // PTR — _remote-hands._tcp.local. -> <host>._remote-hands._tcp.local.
    {
        std::vector<std::uint8_t> rd;
        encode_name(rd, {host, kServiceName, "_tcp", "local"});
        write_record({kServiceName, "_tcp", "local"}, kRrPtr, rd);
    }

    // SRV — priority 0, weight 0, port, target = <host>.local.
    {
        std::vector<std::uint8_t> rd;
        put_u16(rd, 0);                 // priority
        put_u16(rd, 0);                 // weight
        put_u16(rd, tcp_port);
        encode_name(rd, {host, "local"});
        write_record({host, kServiceName, "_tcp", "local"}, kRrSrv, rd);
    }

    // TXT
    {
        std::vector<std::uint8_t> rd;
        const std::string fields[] = {
            "protocol=2",
            "os=" + os_tag,
            "tiers=observe,drive,power",
            "auth=token",
        };
        for (const auto& f : fields) {
            rd.push_back(static_cast<std::uint8_t>(f.size()));
            rd.insert(rd.end(), f.begin(), f.end());
        }
        write_record({host, kServiceName, "_tcp", "local"}, kRrTxt, rd);
    }

    // A — <host>.local. -> ipv4
    {
        std::vector<std::uint8_t> rd;
        rd.push_back(static_cast<std::uint8_t>((ipv4_host_order >> 24) & 0xff));
        rd.push_back(static_cast<std::uint8_t>((ipv4_host_order >> 16) & 0xff));
        rd.push_back(static_cast<std::uint8_t>((ipv4_host_order >>  8) & 0xff));
        rd.push_back(static_cast<std::uint8_t>( ipv4_host_order        & 0xff));
        write_record({host, "local"}, kRrA, rd);
    }

    return msg;
}

}  // namespace

// ---------------------------------------------------------------------------
// Responder::Impl

struct Responder::Impl {
    Config              cfg;
    SOCKET              sock = INVALID_SOCKET;
    std::atomic<bool>   stop_requested{false};
    std::atomic<bool>   started{false};
    std::thread         thread;

    std::string         hostname;
    std::uint32_t       ipv4 = 0;

    explicit Impl(Config c) : cfg{std::move(c)} {}

    void open_socket() {
        sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            log::warning(L"mDNS: socket() failed (%d)", WSAGetLastError());
            return;
        }

        const BOOL reuse = TRUE;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port        = htons(kMulticastPort);
        if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            log::warning(L"mDNS: bind() failed (%d)", WSAGetLastError());
            close_socket();
            return;
        }

        ip_mreq mreq{};
        inet_pton(AF_INET, kMulticastAddr, &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       reinterpret_cast<const char*>(&mreq),
                       sizeof(mreq)) != 0) {
            log::warning(L"mDNS: IP_ADD_MEMBERSHIP failed (%d)",
                         WSAGetLastError());
            // Continue anyway — some Windows configs need a specific
            // interface address rather than INADDR_ANY.
        }

        const DWORD ttl = 255;
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL,
                   reinterpret_cast<const char*>(&ttl), sizeof(ttl));
    }

    void close_socket() {
        if (sock != INVALID_SOCKET) {
            closesocket(sock);
            sock = INVALID_SOCKET;
        }
    }

    void run() {
        hostname = get_hostname_lc();
        ipv4     = find_local_ipv4_host_order();
        if (ipv4 == 0) {
            log::warning(L"mDNS: no usable IPv4 interface; advertising disabled");
            return;
        }

        open_socket();
        if (sock == INVALID_SOCKET) return;

        log::info(L"mDNS responder advertising on %hs._%hs._tcp.local. (port %u)",
                  hostname.c_str(), "remote-hands", cfg.tcp_port);

        sockaddr_in mcast{};
        mcast.sin_family = AF_INET;
        inet_pton(AF_INET, kMulticastAddr, &mcast.sin_addr);
        mcast.sin_port   = htons(kMulticastPort);

        char buf[2048];
        while (!stop_requested.load()) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sock, &rfds);
            timeval tv{0, 100 * 1000};
            const int sel = select(0, &rfds, nullptr, nullptr, &tv);
            if (sel <= 0) continue;

            sockaddr_in src{};
            int srclen = sizeof(src);
            const int n = recvfrom(sock, buf, sizeof(buf), 0,
                                   reinterpret_cast<sockaddr*>(&src), &srclen);
            if (n <= 0) continue;

            // Crude check: does the packet mention our service name? Avoids
            // a full DNS parser; good enough for the basic LAN-discovery
            // case. False positives just mean we send a redundant response.
            const std::string_view bytes{buf, static_cast<std::size_t>(n)};
            if (bytes.find(kServiceName) == std::string_view::npos) continue;

            const auto resp = build_response(hostname, cfg.tcp_port,
                                             cfg.os_tag, ipv4);
            sendto(sock,
                   reinterpret_cast<const char*>(resp.data()),
                   static_cast<int>(resp.size()), 0,
                   reinterpret_cast<sockaddr*>(&mcast), sizeof(mcast));
        }

        close_socket();
    }
};

// ---------------------------------------------------------------------------
// Responder

Responder::Responder(Config cfg) : impl_{std::make_unique<Impl>(std::move(cfg))} {}

Responder::~Responder() {
    stop();
}

void Responder::start() {
    bool expected = false;
    if (!impl_->started.compare_exchange_strong(expected, true)) return;
    impl_->thread = std::thread([this] {
        try {
            impl_->run();
        } catch (const std::exception& ex) {
            log::warning(L"mDNS responder threw: %hs", ex.what());
        }
    });
}

void Responder::stop() {
    impl_->stop_requested.store(true);
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
}

}  // namespace remote_hands::mdns

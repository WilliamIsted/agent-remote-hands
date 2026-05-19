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

#include "server.hpp"

#include "connection.hpp"
#include "errors.hpp"
#include "log.hpp"
#include "power_watcher.hpp"
#include "protocol.hpp"
#include "socket_watchdog.hpp"
#include "token.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <objbase.h>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace remote_hands {

namespace {

std::atomic<long long>& last_activity_atomic() noexcept {
    static std::atomic<long long> v{
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()};
    return v;
}

}  // namespace

long long last_activity_ms() noexcept { return last_activity_atomic().load(); }

void poke_activity() noexcept {
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto& v = last_activity_atomic();
    long long prev = v.load();
    while (prev < now && !v.compare_exchange_weak(prev, now)) {
        // retry on contention
    }
}

struct Server::Impl {
    Config                              config;
    SOCKET                              listen_socket   = INVALID_SOCKET;
    std::shared_ptr<const TokenStore>   token_store;
    std::atomic<int>                    active_connections{0};

    // Rebind coordination: guards listen_socket replacement and the
    // is_rebinding flag. Both the watchdog and power-watcher threads may
    // call rebind() concurrently; the flag ensures only one proceeds.
    std::mutex  rebind_mutex;
    bool        is_rebinding = false;

    explicit Impl(const Config& c) : config{c} {}

    ~Impl() {
        if (listen_socket != INVALID_SOCKET) {
            closesocket(listen_socket);
        }
    }

    void initialise_token_store() {
        token_store = std::make_shared<const TokenStore>(
            TokenStore::initialise(config.token_path, config.token_ttl_hours));
    }

    // Create, configure, bind and listen — returns INVALID_SOCKET on failure.
    // Called by open_listener() (initial startup) and rebind() (post-resume).
    // Replicates all socket options from the original open_listener() path so
    // the replacement socket is identical in configuration.
    SOCKET make_listen_socket() {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return INVALID_SOCKET;

        const int reuse = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port        = htons(config.port);

        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            listen(s, SOMAXCONN) != 0) {
            closesocket(s);
            return INVALID_SOCKET;
        }

        u_long nonblock = 1;
        ioctlsocket(s, FIONBIO, &nonblock);
        return s;
    }

    // Called on PBT_APMSUSPEND: close the listen socket cleanly before the OS
    // tears it down. The accept loop will see WSAEINVAL / WSAENOTSOCK and poll
    // until the rebind path opens a new socket.
    void suspend() {
        log::debug(L"power: PBT_APMSUSPEND -- closing listen socket");
        std::lock_guard<std::mutex> lk(rebind_mutex);
        if (listen_socket != INVALID_SOCKET) {
            closesocket(listen_socket);
            listen_socket = INVALID_SOCKET;
        }
    }

    // Open a replacement listen socket after suspend/resume. Safe to call
    // from the watchdog or power-watcher thread. No-ops if already in
    // progress (is_rebinding flag checked under the lock).
    void rebind() {
        {
            std::lock_guard<std::mutex> lk(rebind_mutex);
            if (is_rebinding) return;
            is_rebinding = true;
        }

        // Retry loop: the network stack may not be fully re-initialised when
        // PBT_APMRESUMEAUTOMATIC fires. WSAENETDOWN / WSAEADDRINUSE are
        // expected on the first few attempts (max 10 s at 500 ms intervals).
        SOCKET new_sock = INVALID_SOCKET;
        for (int attempt = 1; attempt <= 20; ++attempt) {
            log::debug(L"rebind: binding on port %u (attempt %d)",
                       static_cast<unsigned>(config.port), attempt);
            new_sock = make_listen_socket();
            if (new_sock != INVALID_SOCKET) break;
            log::debug(L"rebind: bind failed (%d), retrying in 500ms",
                       WSAGetLastError());
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        {
            std::lock_guard<std::mutex> lk(rebind_mutex);
            if (new_sock != INVALID_SOCKET) {
                if (listen_socket != INVALID_SOCKET) closesocket(listen_socket);
                listen_socket = new_sock;
                watchdog_update_socket(new_sock);
                log::info(L"rebind: listen socket rebound OK");
            } else {
                // Use fprintf so this message survives release builds.
                // Repeated watchdog triggering will re-log every 30 s.
                std::fprintf(stderr,
                             "rebind: FAILED after 20 attempts, "
                             "last error: %d\n",
                             WSAGetLastError());
            }
            is_rebinding = false;
        }
    }

    void open_listener() {
        // Delegate socket creation to make_listen_socket() so the rebind
        // path uses identical configuration (same options, same flags).
        listen_socket = make_listen_socket();
        if (listen_socket == INVALID_SOCKET) {
            throw std::runtime_error("bind() / listen() failed on startup");
        }

        // Print each local IPv4 address so the operator knows what to connect to.
        // gethostname + getaddrinfo is the Winsock 2 / XP SP3+ way; getaddrinfo
        // returns all IPv4 addresses the machine answers on.  Uses inline byte
        // formatting (same pattern as the accept loop) to avoid inet_ntop Vista+
        // dependency for the legacy build.
        {
            char host[256] = {};
            gethostname(host, sizeof(host));
            addrinfo  hints{};
            hints.ai_family   = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* ai      = nullptr;
            log::info(L"Listening on port %u:", config.port);
            if (getaddrinfo(host, nullptr, &hints, &ai) == 0 && ai != nullptr) {
                for (const addrinfo* p = ai; p != nullptr; p = p->ai_next) {
                    const auto* sa = reinterpret_cast<const sockaddr_in*>(p->ai_addr);
                    const auto& b  = sa->sin_addr.S_un.S_un_b;
                    log::info(L"  %u.%u.%u.%u:%u",
                              b.s_b1, b.s_b2, b.s_b3, b.s_b4,
                              static_cast<unsigned>(config.port));
                }
                freeaddrinfo(ai);
            } else {
                log::info(L"  0.0.0.0:%u", static_cast<unsigned>(config.port));
            }
        }
    }

    void accept_loop(std::function<bool()> shutdown_requested) {
        while (!shutdown_requested()) {
            // Snapshot the current listen socket under the lock so we always
            // call accept() on the most recently bound socket (the rebind path
            // replaces listen_socket between polls).
            SOCKET cur;
            {
                std::lock_guard<std::mutex> lk(rebind_mutex);
                cur = listen_socket;
            }

            if (cur == INVALID_SOCKET) {
                // Socket is closed (suspend / rebind in progress): wait.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            sockaddr_in client_addr{};
            int         client_addr_len = sizeof(client_addr);
            const SOCKET client = accept(cur,
                                         reinterpret_cast<sockaddr*>(&client_addr),
                                         &client_addr_len);
            if (client == INVALID_SOCKET) {
                const int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                // WSAEINVAL / WSAENOTSOCK: the socket was replaced by the
                // rebind path (closesocket unblocks a pending accept). This
                // is not a fatal error — loop and pick up the new socket.
                // All other errors are logged at warning level.
                if (err != WSAEINVAL && err != WSAENOTSOCK) {
                    log::warning(L"accept() failed (%d)", err);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            char ipbuf[INET_ADDRSTRLEN] = {};
            // inet_ntop is Vista+; inline byte-format works on XP and all later OS.
            const auto& ab = client_addr.sin_addr.S_un.S_un_b;
            std::snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u",
                          ab.s_b1, ab.s_b2, ab.s_b3, ab.s_b4);
            log::info(L"Accepted connection from %hs:%u",
                      ipbuf, static_cast<unsigned>(ntohs(client_addr.sin_port)));

            // CRITICAL: switch the accepted socket back to blocking mode.
            // accept() on Windows inherits the listener's non-blocking flag
            // (Linux doesn't do this, which is why the bug went unnoticed in
            // local-loopback testing for so long). The listener is non-
            // blocking so the accept loop can poll, but per-connection
            // threads want blocking recv() — without this, the SECOND
            // recv() returns WSAEWOULDBLOCK with no new bytes available,
            // we throw "recv() failed", and the connection dies after the
            // first verb. Hello succeeded only because the bytes were
            // already in the kernel buffer at the first recv() call.
            u_long blocking_mode = 0;
            ioctlsocket(client, FIONBIO, &blocking_mode);

            poke_activity();

            // Per-connection idle-receive timeout (--idle-timeout / env var).
            // SO_RCVTIMEO is in milliseconds; recv() returns WSAETIMEDOUT
            // after the configured period of silence, which the connection
            // loop treats as an EOF and shuts down cleanly.
            if (config.idle_timeout_seconds > 0) {
                const DWORD ms = config.idle_timeout_seconds * 1000U;
                setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                           reinterpret_cast<const char*>(&ms), sizeof(ms));
            }

            if (active_connections.load() >= config.max_connections) {
                refuse_busy(client);
                continue;
            }

            spawn_connection_thread(client);
        }
    }

    void refuse_busy(SOCKET client) noexcept {
        try {
            wire::Writer w{client};
            char detail[48];
            const int n = std::snprintf(detail, sizeof(detail),
                                        "{\"max\":%d}", config.max_connections);
            w.write_err(ErrorCode::Busy,
                        std::string_view{detail, static_cast<std::size_t>(n > 0 ? n : 0)});
        } catch (...) {
            // best-effort: client may have already closed
        }
        closesocket(client);
        log::info(L"Refused connection (busy)");
    }

    void spawn_connection_thread(SOCKET client) {
        active_connections.fetch_add(1);
        std::thread worker([this, client] {
            // Each connection gets its own COM apartment for UIA verbs.
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            try {
                Connection conn{client, token_store, config.max_connections};
                conn.run();
            } catch (const std::exception& ex) {
                log::warning(L"Connection thread exception: %hs", ex.what());
            }
            if (SUCCEEDED(hr)) {
                CoUninitialize();
            }
            active_connections.fetch_sub(1);
        });
        worker.detach();
    }
};

Server::Server(const Config& config) : impl_{std::make_unique<Impl>(config)} {
    impl_->initialise_token_store();
    impl_->open_listener();

    // Start socket-state watchdog. The callback runs on the watchdog thread
    // when the listen socket appears dead; it calls rebind() which is
    // thread-safe.
    watchdog_start([this] { impl_->rebind(); });
    watchdog_update_socket(impl_->listen_socket);

    // Start power-broadcast watcher. The callbacks run on the pump thread.
    power_watcher_start(
        [this] { impl_->suspend(); },
        [this] { impl_->rebind(); });
}

Server::~Server() {
    watchdog_stop();
    power_watcher_stop();
}

void Server::run(std::function<bool()> shutdown_requested) {
    impl_->accept_loop(std::move(shutdown_requested));
}

}  // namespace remote_hands

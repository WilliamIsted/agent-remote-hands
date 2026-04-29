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
#include "protocol.hpp"
#include "token.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <eh.h>          // _set_se_translator
#include <memory>
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

    explicit Impl(const Config& c) : config{c} {}

    ~Impl() {
        if (listen_socket != INVALID_SOCKET) {
            closesocket(listen_socket);
        }
    }

    void initialise_token_store() {
        token_store = std::make_shared<const TokenStore>(
            TokenStore::initialise(config.token_path));
    }

    void open_listener() {
        listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_socket == INVALID_SOCKET) {
            throw std::runtime_error("socket() failed");
        }

        const int reuse = 1;
        setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port        = htons(config.port);

        if (bind(listen_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            throw std::runtime_error("bind() failed");
        }
        if (listen(listen_socket, SOMAXCONN) != 0) {
            throw std::runtime_error("listen() failed");
        }

        u_long nonblock = 1;
        ioctlsocket(listen_socket, FIONBIO, &nonblock);

        log::info(L"Listening on TCP port %u", config.port);
    }

    void accept_loop(std::function<bool()> shutdown_requested) {
        while (!shutdown_requested()) {
            sockaddr_in client_addr{};
            int         client_addr_len = sizeof(client_addr);
            const SOCKET client = accept(listen_socket,
                                         reinterpret_cast<sockaddr*>(&client_addr),
                                         &client_addr_len);
            if (client == INVALID_SOCKET) {
                const int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                log::warning(L"accept() failed (%d)", err);
                continue;
            }

            char ipbuf[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &client_addr.sin_addr, ipbuf, sizeof(ipbuf));
            log::info(L"Accepted connection from %hs:%u",
                      ipbuf, static_cast<unsigned>(ntohs(client_addr.sin_port)));

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
            // Translate Win32 SEH (access violation, stack overflow, integer
            // divide-by-zero, etc.) into C++ runtime_error so the catch
            // below sees them. Without this an access violation anywhere
            // in a verb handler unwinds straight past every C++ try/catch
            // and exits the agent process. Per-thread; needs /EHa from
            // CMakeLists.txt to actually work. (Defence-in-depth fix
            // for #61 — pairs with the subscription cancel in
            // Connection::run.)
            _set_se_translator([](unsigned int code, EXCEPTION_POINTERS* ep) {
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                              "SEH 0x%08X at %p",
                              code,
                              ep ? ep->ExceptionRecord->ExceptionAddress : nullptr);
                throw std::runtime_error(buf);
            });

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
}

Server::~Server() = default;

void Server::run(std::function<bool()> shutdown_requested) {
    impl_->accept_loop(std::move(shutdown_requested));
}

}  // namespace remote_hands

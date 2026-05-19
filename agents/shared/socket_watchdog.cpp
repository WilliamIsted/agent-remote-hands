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

// socket_watchdog.cpp -- defensive listen-socket liveness check.
//
// A background thread wakes periodically and probes the listen socket via
// getsockopt(SO_ERROR). If the socket is dead it invokes the on_dead callback
// (which calls server_rebind()). Silent on a healthy socket.
//
// Do NOT use a self-connect probe: that would create a real accepted
// connection and generate spurious pre-hello-disconnect log noise every
// 30 seconds.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>

#include "socket_watchdog.hpp"
#include "log.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>

namespace remote_hands {

namespace {

std::mutex            g_sock_mtx;
SOCKET                g_sock    = INVALID_SOCKET;
std::atomic<bool>     g_running {false};
std::function<void()> g_on_dead;

void watchdog_loop(unsigned interval_ms) {
    while (g_running.load()) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(interval_ms));

        // Snapshot the socket under the lock to avoid racing with
        // watchdog_update_socket() / server rebind.
        SOCKET s;
        {
            std::lock_guard<std::mutex> lk(g_sock_mtx);
            s = g_sock;
        }
        if (s == INVALID_SOCKET) continue;

        int err = 0;
        int len = static_cast<int>(sizeof(err));
        const bool dead =
            getsockopt(s, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&err), &len) == SOCKET_ERROR
            || err != 0;

        if (dead) {
            log::debug(L"watchdog: listen socket dead (SO_ERROR=%d)"
                       L" -- triggering rebind", err);
            g_on_dead();
        }
        // silent on success
    }
}

}  // namespace

void watchdog_start(std::function<void()> on_dead, unsigned interval_ms) {
    g_on_dead = std::move(on_dead);
    g_running.store(true);
    std::thread([interval_ms] { watchdog_loop(interval_ms); }).detach();
}

void watchdog_update_socket(SOCKET s) {
    std::lock_guard<std::mutex> lk(g_sock_mtx);
    g_sock = s;
}

void watchdog_stop() {
    g_running.store(false);
}

}  // namespace remote_hands

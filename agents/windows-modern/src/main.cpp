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

// Entry point for the windows-modern v2 agent.
//
// Responsibilities:
//   - Parse environment + command line into a Config
//   - Initialise Winsock and COM (apartment-threaded; per-connection threads
//     also init COM in their own apartment)
//   - Install the Ctrl-C handler so shutdown is graceful
//   - Construct the Server and run its accept loop until shutdown is requested
//
// Token-file generation and mDNS responder startup are handled outside this
// file. Installation onto a host (binary placement, firewall rules, Task
// Scheduler registration) lives in `Tools/install-agent.ps1` rather than in
// the agent itself — a self-installing binary trips Microsoft Defender's
// `Program:Win32/Contebrew.A!ml` heuristic, the agent's job is to be a
// wire-protocol server.

#include "config.hpp"
#include "log.hpp"
#include "mdns.hpp"
#include "server.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <winsock2.h>

namespace rh = remote_hands;

namespace {

std::atomic<bool> g_shutdown_requested{false};

BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) noexcept {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_shutdown_requested.store(true);
            return TRUE;
        default:
            return FALSE;
    }
}

// RAII wrapper for WSAStartup / WSACleanup.
class WsaInit {
public:
    WsaInit() {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
    }
    ~WsaInit() noexcept { WSACleanup(); }
    WsaInit(const WsaInit&) = delete;
    WsaInit& operator=(const WsaInit&) = delete;
};

// RAII wrapper for COM apartment init.
class ComInit {
public:
    ComInit() {
        // Apartment-threaded for UIA on the main thread; per-connection threads
        // initialise their own apartments as needed.
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hr)) {
            throw std::runtime_error("CoInitializeEx failed");
        }
    }
    ~ComInit() noexcept { CoUninitialize(); }
    ComInit(const ComInit&) = delete;
    ComInit& operator=(const ComInit&) = delete;
};

}  // namespace

int wmain(int argc, wchar_t* argv[]) try {
    auto config = rh::Config::parse(argc, argv);

    rh::log::info(L"Agent Remote Hands v2.0 starting on TCP port %u", config.port);

    WsaInit wsa;
    ComInit com;

    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    // Optional mDNS advertisement (`-Discoverable` / REMOTE_HANDS_DISCOVERABLE=1).
    std::unique_ptr<rh::mdns::Responder> mdns_responder;
    if (config.discoverable) {
        rh::mdns::Config mdns_cfg{};
        mdns_cfg.tcp_port = config.port;
        mdns_responder = std::make_unique<rh::mdns::Responder>(std::move(mdns_cfg));
        mdns_responder->start();
    }

    rh::Server server{config};

    // Optional watchdog (`--watchdog <s>` / REMOTE_HANDS_WATCHDOG): self-exits
    // the agent if no connection activity has been seen for the configured
    // window. Pair with Task Scheduler restart-on-failure for unattended
    // recovery from wedged states.
    std::thread watchdog_thread;
    if (config.watchdog_seconds > 0) {
        const auto window_ms = static_cast<long long>(config.watchdog_seconds) * 1000LL;
        rh::log::info(L"Watchdog enabled: self-exit after %us idle",
                      config.watchdog_seconds);
        watchdog_thread = std::thread([window_ms] {
            while (!g_shutdown_requested.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                const long long now =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                if (now - rh::last_activity_ms() > window_ms) {
                    rh::log::warning(L"Watchdog: no activity for the configured "
                                     L"window; requesting shutdown");
                    g_shutdown_requested.store(true);
                    return;
                }
            }
        });
    }

    server.run([] { return g_shutdown_requested.load(); });

    if (watchdog_thread.joinable()) watchdog_thread.join();

    if (mdns_responder) {
        mdns_responder->stop();
        mdns_responder.reset();
    }

    rh::log::info(L"Agent shutting down cleanly");
    return 0;
} catch (const std::exception& ex) {
    std::cerr << "Fatal: " << ex.what() << "\n";
    return 1;
}

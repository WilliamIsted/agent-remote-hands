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
// Token-file generation, mDNS responder startup, and the install / uninstall
// paths are handled outside this file (see config.cpp for parsing, and the
// matching modules added in subsequent build phases).

#include "config.hpp"
#include "install.hpp"
#include "log.hpp"
#include "mdns.hpp"
#include "server.hpp"

#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>

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

    if (config.install_mode == rh::InstallMode::Install) {
        return rh::install::run_install(config);
    }
    if (config.install_mode == rh::InstallMode::Uninstall) {
        return rh::install::run_uninstall(config);
    }

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
    server.run([] { return g_shutdown_requested.load(); });

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

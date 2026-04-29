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

#include "config.hpp"

#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

namespace remote_hands {

namespace {

std::filesystem::path default_token_path() {
    PWSTR program_data = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &program_data))) {
        std::filesystem::path p{program_data};
        CoTaskMemFree(program_data);
        p /= L"AgentRemoteHands";
        p /= L"token";
        return p;
    }
    // Fallback if SHGetKnownFolderPath fails for any reason.
    return L"C:\\ProgramData\\AgentRemoteHands\\token";
}

std::uint16_t parse_port(const wchar_t* s) {
    const unsigned long v = std::wcstoul(s, nullptr, 10);
    if (v == 0 || v > 65535) {
        throw std::runtime_error("Port must be 1..65535");
    }
    return static_cast<std::uint16_t>(v);
}

// RAII wrapper around `_wdupenv_s`. Returns nullptr if the variable is unset
// or allocation fails; the buffer is freed automatically when the unique_ptr
// goes out of scope.
using EnvBuf = std::unique_ptr<wchar_t, decltype(&std::free)>;

EnvBuf get_env(const wchar_t* name) {
    wchar_t* raw = nullptr;
    size_t   len = 0;
    if (_wdupenv_s(&raw, &len, name) != 0) raw = nullptr;
    return EnvBuf{raw, std::free};
}

[[noreturn]] void print_usage_and_exit() {
    std::wprintf(LR"(Agent Remote Hands - windows-modern v2 agent

Usage: remote-hands.exe [options]

Options:
  --port <n>              TCP port to listen on (default: 8765)
  --discoverable          Advertise via mDNS (_remote-hands._tcp.local.)
  --token-path <path>     Path to the elevation token file
                          (default: %%ProgramData%%\AgentRemoteHands\token)
  --max-connections <n>   Concurrent connection cap (default: 4)
  --idle-timeout <s>      Drop connections idle for this many seconds (default: 0 = off)
  --watchdog <s>          Self-exit if no connection activity for this many
                          seconds (default: 0 = off; pair with Task Scheduler
                          restart-on-failure for unattended recovery)
  -h, --help              Show this help and exit

Environment variables (overridden by CLI flags):
  REMOTE_HANDS_PORT
  REMOTE_HANDS_DISCOVERABLE   (set to "1" to enable)
  REMOTE_HANDS_TOKEN_PATH
  REMOTE_HANDS_IDLE_TIMEOUT   (seconds)
  REMOTE_HANDS_WATCHDOG       (seconds)

Install / uninstall: see Tools\install-agent.ps1 (run from elevated
PowerShell). The agent itself is a wire-protocol server, not an installer.
)");
    std::exit(0);
}

}  // namespace

Config Config::parse(int argc, wchar_t* argv[]) {
    Config c;
    c.token_path = default_token_path();

    // Environment first.
    if (auto env_port = get_env(L"REMOTE_HANDS_PORT"); env_port && *env_port) {
        c.port = parse_port(env_port.get());
    }
    if (auto env_disc = get_env(L"REMOTE_HANDS_DISCOVERABLE"); env_disc) {
        c.discoverable = (std::wstring_view{env_disc.get()} == L"1");
    }
    if (auto env_token = get_env(L"REMOTE_HANDS_TOKEN_PATH"); env_token && *env_token) {
        c.token_path = env_token.get();
    }
    if (auto env_idle = get_env(L"REMOTE_HANDS_IDLE_TIMEOUT"); env_idle && *env_idle) {
        c.idle_timeout_seconds = static_cast<unsigned int>(
            std::wcstoul(env_idle.get(), nullptr, 10));
    }
    if (auto env_wd = get_env(L"REMOTE_HANDS_WATCHDOG"); env_wd && *env_wd) {
        c.watchdog_seconds = static_cast<unsigned int>(
            std::wcstoul(env_wd.get(), nullptr, 10));
    }

    // CLI overrides.
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg{argv[i]};
        if (arg == L"--port" && i + 1 < argc) {
            c.port = parse_port(argv[++i]);
        } else if (arg == L"--discoverable") {
            c.discoverable = true;
        } else if (arg == L"--token-path" && i + 1 < argc) {
            c.token_path = argv[++i];
        } else if (arg == L"--max-connections" && i + 1 < argc) {
            const auto v = std::wcstoul(argv[++i], nullptr, 10);
            if (v == 0 || v > 1024) {
                throw std::runtime_error("--max-connections must be 1..1024");
            }
            c.max_connections = static_cast<int>(v);
        } else if (arg == L"--idle-timeout" && i + 1 < argc) {
            c.idle_timeout_seconds = static_cast<unsigned int>(
                std::wcstoul(argv[++i], nullptr, 10));
        } else if (arg == L"--watchdog" && i + 1 < argc) {
            c.watchdog_seconds = static_cast<unsigned int>(
                std::wcstoul(argv[++i], nullptr, 10));
        } else if (arg == L"--install" || arg == L"--uninstall") {
            std::fwprintf(stderr,
                L"--install / --uninstall were removed in this build.\n"
                L"Use Tools\\install-agent.ps1 (elevated PowerShell). See README.\n");
            std::exit(2);
        } else if (arg == L"--help" || arg == L"-h") {
            print_usage_and_exit();
        } else {
            throw std::runtime_error("Unknown argument; use --help for usage");
        }
    }

    return c;
}

}  // namespace remote_hands

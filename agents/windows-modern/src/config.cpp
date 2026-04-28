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

[[noreturn]] void print_usage_and_exit() {
    std::wprintf(LR"(Agent Remote Hands - windows-modern v2 agent

Usage: remote-hands.exe [options]

Options:
  --port <n>              TCP port to listen on (default: 8765)
  --discoverable          Advertise via mDNS (_remote-hands._tcp.local.)
  --token-path <path>     Path to the elevation token file
                          (default: %%ProgramData%%\AgentRemoteHands\token)
  --max-connections <n>   Concurrent connection cap (default: 4)
  --install               Register the autostart logon-task and exit
  --uninstall             Remove the autostart task and exit
  -h, --help              Show this help and exit

Environment variables (overridden by CLI flags):
  REMOTE_HANDS_PORT
  REMOTE_HANDS_DISCOVERABLE   (set to "1" to enable)
  REMOTE_HANDS_TOKEN_PATH
)");
    std::exit(0);
}

}  // namespace

Config Config::parse(int argc, wchar_t* argv[]) {
    Config c;
    c.token_path = default_token_path();

    // Environment first.
    if (auto* env_port = _wgetenv(L"REMOTE_HANDS_PORT"); env_port && *env_port) {
        c.port = parse_port(env_port);
    }
    if (auto* env_disc = _wgetenv(L"REMOTE_HANDS_DISCOVERABLE"); env_disc) {
        c.discoverable = (std::wstring_view{env_disc} == L"1");
    }
    if (auto* env_token = _wgetenv(L"REMOTE_HANDS_TOKEN_PATH"); env_token && *env_token) {
        c.token_path = env_token;
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
        } else if (arg == L"--install") {
            c.install_mode = InstallMode::Install;
        } else if (arg == L"--uninstall") {
            c.install_mode = InstallMode::Uninstall;
        } else if (arg == L"--help" || arg == L"-h") {
            print_usage_and_exit();
        } else {
            throw std::runtime_error("Unknown argument; use --help for usage");
        }
    }

    return c;
}

}  // namespace remote_hands

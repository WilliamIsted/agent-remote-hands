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

#pragma once

#include <cstdint>
#include <filesystem>

namespace remote_hands {

enum class InstallMode {
    Run,        // Normal operation: listen and serve
    Install,    // --install: register Task Scheduler logon-task and exit
    Uninstall,  // --uninstall: remove the task and exit
};

// Parsed configuration from environment variables and command-line flags.
//
// Precedence: CLI flags override environment, environment overrides defaults.
struct Config {
    std::uint16_t           port            = 8765;
    bool                    discoverable    = false;
    std::filesystem::path   token_path;       // Default: %ProgramData%\AgentRemoteHands\token
    int                     max_connections = 4;
    InstallMode             install_mode    = InstallMode::Run;

    // Parses argc / argv (wide-char) and returns a populated Config.
    // Exits the process on --help. Throws std::runtime_error on unknown flags.
    static Config parse(int argc, wchar_t* argv[]);
};

}  // namespace remote_hands

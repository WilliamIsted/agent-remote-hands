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

// Parsed configuration from environment variables and command-line flags.
//
// Precedence: CLI flags override environment, environment overrides defaults.
//
// The agent is purely a wire-protocol server. Installation onto a host
// (binary placement, firewall rules, Task Scheduler registration) lives in
// `Tools/install-agent.ps1`, not in this binary — see the README.
struct Config {
    std::uint16_t           port            = 8765;
    bool                    discoverable    = false;
    std::filesystem::path   token_path;       // Default: %ProgramData%\AgentRemoteHands\token
    int                     max_connections = 4;
    // Per-connection idle-receive timeout in seconds. Connections with no
    // activity for longer than this are dropped; 0 disables.
    unsigned int            idle_timeout_seconds = 0;
    // Whole-agent watchdog in seconds. If no connection activity occurs for
    // longer than this, the agent self-exits (Task Scheduler is then expected
    // to restart it). 0 disables.
    unsigned int            watchdog_seconds     = 0;

    // Parses argc / argv (wide-char) and returns a populated Config.
    // Exits the process on --help. Throws std::runtime_error on unknown flags.
    static Config parse(int argc, wchar_t* argv[]);
};

}  // namespace remote_hands

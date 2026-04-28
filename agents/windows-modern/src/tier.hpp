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

// Three-tier permission model per PROTOCOL.md §7.
//
//   observe  -- read-only operations
//   drive    -- includes synthetic input, file writes, focus changes
//   power    -- includes destructive ops: kill, delete, reboot

#include <optional>
#include <string_view>

namespace remote_hands {

enum class Tier {
    Observe = 0,
    Drive   = 1,
    Power   = 2,
};

constexpr std::string_view to_wire(Tier t) noexcept {
    switch (t) {
        case Tier::Observe: return "observe";
        case Tier::Drive:   return "drive";
        case Tier::Power:   return "power";
    }
    return "unknown";
}

constexpr std::optional<Tier> tier_from_wire(std::string_view sv) noexcept {
    if (sv == "observe") return Tier::Observe;
    if (sv == "drive")   return Tier::Drive;
    if (sv == "power")   return Tier::Power;
    return std::nullopt;
}

// Returns true if a connection at `current` may invoke a verb requiring `required`.
constexpr bool tier_satisfies(Tier required, Tier current) noexcept {
    return static_cast<int>(current) >= static_cast<int>(required);
}

}  // namespace remote_hands

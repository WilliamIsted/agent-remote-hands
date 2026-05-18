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

// Five-tier permission ladder per PROTOCOL.md §7. Letters match CRUDX.
//
//   read         (R)  -- observe state without changing it (default tier)
//   create       (C)  -- bring something new into existence (mkdir, process.start)
//   update       (U)  -- overwrite or move existing things (input synthesis, file write, focus)
//   delete       (D)  -- make existing things cease to be (file.delete, process.kill)
//   extra_risky  (X)  -- affect connection / system / power state (shutdown, reboot)
//
// Strict ladder: holding a higher tier subsumes every lower tier.

#include <optional>
#include <string_view>

namespace remote_hands {

enum class Tier {
    Read        = 0,
    Create      = 1,
    Update      = 2,
    Delete      = 3,
    ExtraRisky  = 4,
};

constexpr std::string_view to_wire(Tier t) noexcept {
    switch (t) {
        case Tier::Read:       return "read";
        case Tier::Create:     return "create";
        case Tier::Update:     return "update";
        case Tier::Delete:     return "delete";
        case Tier::ExtraRisky: return "extra_risky";
    }
    return "unknown";
}

constexpr std::optional<Tier> tier_from_wire(std::string_view sv) noexcept {
    if (sv == "read")        return Tier::Read;
    if (sv == "create")      return Tier::Create;
    if (sv == "update")      return Tier::Update;
    if (sv == "delete")      return Tier::Delete;
    if (sv == "extra_risky") return Tier::ExtraRisky;
    return std::nullopt;
}

// Returns true if a connection at `current` may invoke a verb requiring `required`.
constexpr bool tier_satisfies(Tier required, Tier current) noexcept {
    return static_cast<int>(current) >= static_cast<int>(required);
}

}  // namespace remote_hands

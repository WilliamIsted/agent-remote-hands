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

// Verb dispatch table and capability advertisement.
//
// Each verb (other than the connection.* lifecycle set) is registered in the
// table here with its required tier and a free-function handler. The
// connection state machine looks up a verb, enforces the tier, and invokes
// the handler.
//
// Adding a verb:
//   1. Implement it in `verbs/<namespace>.cpp` with signature
//        void name(Connection&, const wire::Request&);
//   2. Forward-declare it inside capabilities.cpp's anonymous namespace.
//   3. Add an entry to the kVerbs table in capabilities.cpp.
//
// `system.capabilities` is built from the same table — adding a verb
// automatically advertises it.

#include "tier.hpp"

#include <string>
#include <string_view>

namespace remote_hands {

class Connection;
namespace wire { struct Request; }

using VerbHandler = void (*)(Connection&, const wire::Request&);

struct VerbEntry {
    Tier        required_tier;
    VerbHandler handler;
};

// Looks up a verb by name. Returns nullptr if unknown to this build.
const VerbEntry* find_verb(std::string_view verb);

// Builds the JSON body for the `system.capabilities` response.
std::string build_capabilities_json();

// Builds the comma-separated namespaces array (for `system.info.namespaces`).
// Always includes "connection" — the lifecycle namespace is implicit.
std::string build_namespaces_json_array();

}  // namespace remote_hands

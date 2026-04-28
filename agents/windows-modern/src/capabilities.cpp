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

#include "capabilities.hpp"

#include "connection.hpp"

#include <set>
#include <string>
#include <string_view>
#include <unordered_map>

namespace remote_hands {

// ---------------------------------------------------------------------------
// Verb forward declarations.
//
// Each verb's implementation lives in `verbs/<namespace>.cpp` and is forward-
// declared here so the dispatch table can name it.

namespace system_verbs {
    void info(Connection&, const wire::Request&);
    void capabilities(Connection&, const wire::Request&);
    void health(Connection&, const wire::Request&);
    void lock(Connection&, const wire::Request&);
    void shutdown_blockers(Connection&, const wire::Request&);
    void reboot(Connection&, const wire::Request&);
    void shutdown(Connection&, const wire::Request&);
    void logoff(Connection&, const wire::Request&);
    void hibernate(Connection&, const wire::Request&);
    void sleep(Connection&, const wire::Request&);
}  // namespace system_verbs

namespace window_verbs {
    void list(Connection&, const wire::Request&);
    void find(Connection&, const wire::Request&);
    void focus(Connection&, const wire::Request&);
    void close(Connection&, const wire::Request&);
    void move(Connection&, const wire::Request&);
    void state(Connection&, const wire::Request&);
}  // namespace window_verbs

// ---------------------------------------------------------------------------

namespace {

const std::unordered_map<std::string_view, VerbEntry>& verb_table() {
    static const std::unordered_map<std::string_view, VerbEntry> kVerbs{
        // system.*
        {"system.info",                {Tier::Observe, &system_verbs::info}},
        {"system.capabilities",        {Tier::Observe, &system_verbs::capabilities}},
        {"system.health",              {Tier::Observe, &system_verbs::health}},
        {"system.lock",                {Tier::Observe, &system_verbs::lock}},
        {"system.shutdown_blockers",   {Tier::Observe, &system_verbs::shutdown_blockers}},
        {"system.reboot",              {Tier::Power,   &system_verbs::reboot}},
        {"system.shutdown",            {Tier::Power,   &system_verbs::shutdown}},
        {"system.logoff",              {Tier::Power,   &system_verbs::logoff}},
        {"system.hibernate",           {Tier::Power,   &system_verbs::hibernate}},
        {"system.sleep",               {Tier::Power,   &system_verbs::sleep}},

        // window.*
        {"window.list",                {Tier::Observe, &window_verbs::list}},
        {"window.find",                {Tier::Observe, &window_verbs::find}},
        {"window.focus",               {Tier::Drive,   &window_verbs::focus}},
        {"window.close",               {Tier::Drive,   &window_verbs::close}},
        {"window.move",                {Tier::Drive,   &window_verbs::move}},
        {"window.state",               {Tier::Observe, &window_verbs::state}},

        // Future phases register screen.*, input.*, element.*, file.*,
        // process.*, registry.*, clipboard.*, watch.* here.
    };
    return kVerbs;
}

}  // namespace

const VerbEntry* find_verb(std::string_view verb) {
    const auto& tbl = verb_table();
    if (auto it = tbl.find(verb); it != tbl.end()) {
        return &it->second;
    }
    return nullptr;
}

std::string build_capabilities_json() {
    std::string out;
    out += '{';
    bool first = true;
    for (const auto& [name, entry] : verb_table()) {
        if (!first) out += ',';
        first = false;
        out += '"'; out.append(name); out += "\":{\"tier\":\"";
        out.append(to_wire(entry.required_tier));
        out += "\"}";
    }
    out += '}';
    return out;
}

std::string build_namespaces_json_array() {
    std::set<std::string_view> namespaces;
    namespaces.insert("connection");
    for (const auto& [name, _] : verb_table()) {
        const auto dot = name.find('.');
        if (dot != std::string_view::npos) {
            namespaces.insert(name.substr(0, dot));
        }
    }

    std::string out;
    out += '[';
    bool first = true;
    for (const auto& ns : namespaces) {
        if (!first) out += ',';
        first = false;
        out += '"'; out.append(ns); out += '"';
    }
    out += ']';
    return out;
}

}  // namespace remote_hands

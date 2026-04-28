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

namespace input_verbs {
    void click(Connection&, const wire::Request&);
    void move(Connection&, const wire::Request&);
    void scroll(Connection&, const wire::Request&);
    void key(Connection&, const wire::Request&);
    void type(Connection&, const wire::Request&);
    void send_message(Connection&, const wire::Request&);
}  // namespace input_verbs

namespace clipboard_verbs {
    void read(Connection&, const wire::Request&);
    void write(Connection&, const wire::Request&);
}  // namespace clipboard_verbs

namespace registry_verbs {
    void read(Connection&, const wire::Request&);
    void write(Connection&, const wire::Request&);
    void delete_(Connection&, const wire::Request&);
    void wait(Connection&, const wire::Request&);
}  // namespace registry_verbs

namespace process_verbs {
    void list(Connection&, const wire::Request&);
    void start(Connection&, const wire::Request&);
    void shell(Connection&, const wire::Request&);
    void kill(Connection&, const wire::Request&);
    void wait(Connection&, const wire::Request&);
}  // namespace process_verbs

namespace file_verbs {
    void read(Connection&, const wire::Request&);
    void write(Connection&, const wire::Request&);
    void list(Connection&, const wire::Request&);
    void stat(Connection&, const wire::Request&);
    void delete_(Connection&, const wire::Request&);
    void exists(Connection&, const wire::Request&);
    void wait(Connection&, const wire::Request&);
    void mkdir(Connection&, const wire::Request&);
    void rename(Connection&, const wire::Request&);
}  // namespace file_verbs

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

        // input.*
        {"input.click",                {Tier::Drive,   &input_verbs::click}},
        {"input.move",                 {Tier::Drive,   &input_verbs::move}},
        {"input.scroll",               {Tier::Drive,   &input_verbs::scroll}},
        {"input.key",                  {Tier::Drive,   &input_verbs::key}},
        {"input.type",                 {Tier::Drive,   &input_verbs::type}},
        {"input.send_message",         {Tier::Drive,   &input_verbs::send_message}},

        // clipboard.*
        {"clipboard.read",             {Tier::Observe, &clipboard_verbs::read}},
        {"clipboard.write",            {Tier::Drive,   &clipboard_verbs::write}},

        // registry.*
        {"registry.read",              {Tier::Observe, &registry_verbs::read}},
        {"registry.write",             {Tier::Drive,   &registry_verbs::write}},
        {"registry.delete",            {Tier::Power,   &registry_verbs::delete_}},
        {"registry.wait",              {Tier::Observe, &registry_verbs::wait}},

        // process.*
        {"process.list",               {Tier::Observe, &process_verbs::list}},
        {"process.start",              {Tier::Drive,   &process_verbs::start}},
        {"process.shell",              {Tier::Drive,   &process_verbs::shell}},
        {"process.kill",               {Tier::Power,   &process_verbs::kill}},
        {"process.wait",               {Tier::Observe, &process_verbs::wait}},

        // file.*
        {"file.read",                  {Tier::Observe, &file_verbs::read}},
        {"file.write",                 {Tier::Drive,   &file_verbs::write}},
        {"file.list",                  {Tier::Observe, &file_verbs::list}},
        {"file.stat",                  {Tier::Observe, &file_verbs::stat}},
        {"file.delete",                {Tier::Power,   &file_verbs::delete_}},
        {"file.exists",                {Tier::Observe, &file_verbs::exists}},
        {"file.wait",                  {Tier::Observe, &file_verbs::wait}},
        {"file.mkdir",                 {Tier::Drive,   &file_verbs::mkdir}},
        {"file.rename",                {Tier::Drive,   &file_verbs::rename}},

        // Future phases register screen.*, element.*, watch.* here.
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

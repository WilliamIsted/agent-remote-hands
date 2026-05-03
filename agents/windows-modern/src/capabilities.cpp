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
    void power_cancel(Connection&, const wire::Request&);
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
    void post_message(Connection&, const wire::Request&);
}  // namespace input_verbs

namespace clipboard_verbs {
    void get(Connection&, const wire::Request&);
    void set(Connection&, const wire::Request&);
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
    void write_at(Connection&, const wire::Request&);
    void stat(Connection&, const wire::Request&);
    void delete_(Connection&, const wire::Request&);
    void exists(Connection&, const wire::Request&);
    void wait(Connection&, const wire::Request&);
    void rename(Connection&, const wire::Request&);
    // Directory-namespace handlers live in verbs/file.cpp (single translation
    // unit for fs ops). Wire names map onto these symbols.
    void list(Connection&, const wire::Request&);              // wire: directory.list
    void mkdir(Connection&, const wire::Request&);             // wire: directory.create
    void directory_stat(Connection&, const wire::Request&);    // wire: directory.stat
    void directory_exists(Connection&, const wire::Request&);  // wire: directory.exists
    void directory_rename(Connection&, const wire::Request&);  // wire: directory.rename
    void directory_remove(Connection&, const wire::Request&);  // wire: directory.remove
}  // namespace file_verbs

namespace element_verbs {
    void list(Connection&, const wire::Request&);
    void tree(Connection&, const wire::Request&);
    void at(Connection&, const wire::Request&);
    void find(Connection&, const wire::Request&);
    void wait(Connection&, const wire::Request&);
    void find_invoke(Connection&, const wire::Request&);
    void at_invoke(Connection&, const wire::Request&);
    void invoke(Connection&, const wire::Request&);
    void toggle(Connection&, const wire::Request&);
    void expand(Connection&, const wire::Request&);
    void collapse(Connection&, const wire::Request&);
    void focus(Connection&, const wire::Request&);
    void text(Connection&, const wire::Request&);
    void set_text(Connection&, const wire::Request&);
}  // namespace element_verbs

namespace screen_verbs {
    void capture(Connection&, const wire::Request&);
}  // namespace screen_verbs

namespace watch_verbs {
    void region(Connection&, const wire::Request&);
    void process(Connection&, const wire::Request&);
    void window(Connection&, const wire::Request&);
    void element(Connection&, const wire::Request&);
    void file(Connection&, const wire::Request&);
    void registry(Connection&, const wire::Request&);
    void cancel(Connection&, const wire::Request&);
}  // namespace watch_verbs

// ---------------------------------------------------------------------------

namespace {

const std::unordered_map<std::string_view, VerbEntry>& verb_table() {
    static const std::unordered_map<std::string_view, VerbEntry> kVerbs{
        // system.*
        {"system.info",                {Tier::Read,       &system_verbs::info}},
        {"system.capabilities",        {Tier::Read,       &system_verbs::capabilities}},
        {"system.health",              {Tier::Read,       &system_verbs::health}},
        {"system.lock",                {Tier::Read,       &system_verbs::lock}},
        {"system.shutdown_blockers",   {Tier::Read,       &system_verbs::shutdown_blockers}},
        {"system.reboot",              {Tier::ExtraRisky, &system_verbs::reboot}},
        {"system.shutdown",            {Tier::ExtraRisky, &system_verbs::shutdown}},
        {"system.logoff",              {Tier::ExtraRisky, &system_verbs::logoff}},
        {"system.hibernate",           {Tier::ExtraRisky, &system_verbs::hibernate}},
        {"system.sleep",               {Tier::ExtraRisky, &system_verbs::sleep}},
        {"system.power.cancel",        {Tier::ExtraRisky, &system_verbs::power_cancel}},

        // window.*
        {"window.list",                {Tier::Read,       &window_verbs::list}},
        {"window.find",                {Tier::Read,       &window_verbs::find}},
        {"window.focus",               {Tier::Update,     &window_verbs::focus}},
        {"window.close",               {Tier::Update,     &window_verbs::close}},
        {"window.move",                {Tier::Update,     &window_verbs::move}},
        {"window.state",               {Tier::Read,       &window_verbs::state}},

        // input.*
        {"input.click",                {Tier::Update,     &input_verbs::click}},
        {"input.move",                 {Tier::Update,     &input_verbs::move}},
        {"input.scroll",               {Tier::Update,     &input_verbs::scroll}},
        {"input.key",                  {Tier::Update,     &input_verbs::key}},
        {"input.type",                 {Tier::Update,     &input_verbs::type}},
        {"input.send_message",         {Tier::Update,     &input_verbs::send_message}},
        {"input.post_message",         {Tier::Update,     &input_verbs::post_message}},

        // clipboard.*
        {"clipboard.get",              {Tier::Read,       &clipboard_verbs::get}},
        {"clipboard.set",              {Tier::Update,     &clipboard_verbs::set}},

        // registry.*
        {"registry.read",              {Tier::Read,       &registry_verbs::read}},
        {"registry.write",             {Tier::Update,     &registry_verbs::write}},
        {"registry.delete",            {Tier::Delete,     &registry_verbs::delete_}},
        {"registry.wait",              {Tier::Read,       &registry_verbs::wait}},

        // process.*
        {"process.list",               {Tier::Read,       &process_verbs::list}},
        {"process.start",              {Tier::Create,     &process_verbs::start}},
        {"process.shell",              {Tier::Create,     &process_verbs::shell}},
        {"process.kill",               {Tier::Delete,     &process_verbs::kill}},
        {"process.wait",               {Tier::Read,       &process_verbs::wait}},

        // file.*
        {"file.read",                  {Tier::Read,       &file_verbs::read}},
        {"file.write",                 {Tier::Update,     &file_verbs::write}},
        {"file.write_at",              {Tier::Update,     &file_verbs::write_at}},
        {"file.stat",                  {Tier::Read,       &file_verbs::stat}},
        {"file.delete",                {Tier::Delete,     &file_verbs::delete_}},
        {"file.exists",                {Tier::Read,       &file_verbs::exists}},
        {"file.wait",                  {Tier::Read,       &file_verbs::wait}},
        {"file.rename",                {Tier::Update,     &file_verbs::rename}},

        // directory.* (split out of file.* in v2.1; new primitives added)
        {"directory.list",             {Tier::Read,       &file_verbs::list}},
        {"directory.stat",             {Tier::Read,       &file_verbs::directory_stat}},
        {"directory.exists",           {Tier::Read,       &file_verbs::directory_exists}},
        {"directory.create",           {Tier::Create,     &file_verbs::mkdir}},
        {"directory.rename",           {Tier::Update,     &file_verbs::directory_rename}},
        {"directory.remove",           {Tier::Delete,     &file_verbs::directory_remove}},

        // element.*
        {"element.list",               {Tier::Read,       &element_verbs::list}},
        {"element.tree",               {Tier::Read,       &element_verbs::tree}},
        {"element.at",                 {Tier::Read,       &element_verbs::at}},
        {"element.find",               {Tier::Read,       &element_verbs::find}},
        {"element.wait",               {Tier::Read,       &element_verbs::wait}},
        {"element.find_invoke",        {Tier::Update,     &element_verbs::find_invoke}},
        {"element.at_invoke",          {Tier::Update,     &element_verbs::at_invoke}},
        {"element.invoke",             {Tier::Update,     &element_verbs::invoke}},
        {"element.toggle",             {Tier::Update,     &element_verbs::toggle}},
        {"element.expand",             {Tier::Update,     &element_verbs::expand}},
        {"element.collapse",           {Tier::Update,     &element_verbs::collapse}},
        {"element.focus",              {Tier::Update,     &element_verbs::focus}},
        {"element.text",               {Tier::Read,       &element_verbs::text}},
        {"element.set_text",           {Tier::Update,     &element_verbs::set_text}},

        // screen.*
        {"screen.capture",             {Tier::Read,       &screen_verbs::capture}},

        // watch.*
        {"watch.region",               {Tier::Read,       &watch_verbs::region}},
        {"watch.process",              {Tier::Read,       &watch_verbs::process}},
        {"watch.window",               {Tier::Read,       &watch_verbs::window}},
        {"watch.element",              {Tier::Read,       &watch_verbs::element}},
        {"watch.file",                 {Tier::Read,       &watch_verbs::file}},
        {"watch.registry",             {Tier::Read,       &watch_verbs::registry}},
        {"watch.cancel",               {Tier::Read,       &watch_verbs::cancel}},
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

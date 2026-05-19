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

// Named-argument accessors over wire::Request (Phase 2.0 strangler-fig
// foundation).
//
// MCP framing (§1.6.3) delivers a NAMED `arguments` JSON object keyed by each
// verb's input_schema properties. Phase 1 bridged this by flattening the
// object back into positional `--key value` tokens (mcp_session.cpp's
// map_arguments). Phase 2.0 ALSO attaches the parsed object to
// wire::Request::named_root; these accessors give handlers a typed,
// schema-shaped view of it so Phase 2.1 can migrate each namespace's handlers
// off positional parsing one at a time.
//
// LAYERING: this header pulls in the MCP JSON parser, so ONLY translation
// units that already depend on the MCP module may include it. protocol.hpp
// keeps the named view as an opaque shared_ptr<const mcp::JsonValue> and never
// includes this. Phase 2.0 deliberately does NOT call any of these from a verb
// handler yet — the positional path is still authoritative.
//
// Semantics when named_root is null (the ARH bootstrap / legacy-classic
// header path, or any non-object arguments value): every accessor reports
// "absent" — has() is false, the optional getters return std::nullopt,
// arg_node() returns nullptr. Handlers that have been migrated detect this and
// fall back to positional `args` until Phase 2.1 retires that path.

#include "../mcp/json_parse.hpp"
#include "../protocol.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace remote_hands::wire {

// The root named-args object for this request, or nullptr when the request
// did not arrive over MCP framing (or arguments was not a JSON object).
inline const mcp::JsonValue* named_args(const Request& req) noexcept {
    const mcp::JsonValue* root = req.named_root.get();
    if (root == nullptr || !root->is_object()) {
        return nullptr;
    }
    return root;
}

// Raw node lookup. Returns the JsonValue for `key` (any JSON kind — useful
// for nested object/array shapes like region:{x,y,w,h} or modifiers:[...]),
// or nullptr if the named view is absent or the key is not present.
inline const mcp::JsonValue* arg_node(const Request& req,
                                       std::string_view key) noexcept {
    const mcp::JsonValue* root = named_args(req);
    return root ? root->find(key) : nullptr;
}

// True iff the named view carries `key` with a non-null value. (A JSON null
// is treated as "absent" — it mirrors map_arguments(), which drops null-valued
// keys, so the two arg models agree during the migration.)
inline bool has(const Request& req, std::string_view key) noexcept {
    const mcp::JsonValue* n = arg_node(req, key);
    return n != nullptr && !n->is_null();
}

// String accessor. Present-and-string -> the string; otherwise nullopt.
inline std::optional<std::string> arg_str(const Request& req,
                                           std::string_view key) {
    const mcp::JsonValue* n = arg_node(req, key);
    if (n == nullptr || !n->is_string()) {
        return std::nullopt;
    }
    return n->as_string();
}

// Integer accessor. Present-and-number -> truncated toward zero as long long.
// (The parser keeps the exact lexeme; callers that need the fractional part
// use arg_node() and read as_number() directly. JSON has one number type, so
// "is this an int" is a caller concern, not the wire's.) A JSON bool is NOT
// coerced — booleans go through arg_bool().
inline std::optional<long long> arg_int(const Request& req,
                                         std::string_view key) {
    const mcp::JsonValue* n = arg_node(req, key);
    if (n == nullptr || !n->is_number()) {
        return std::nullopt;
    }
    return static_cast<long long>(n->as_number());
}

// Boolean accessor. Present-and-bool -> the bool; otherwise nullopt. (No
// truthy coercion of strings/numbers — the schema declares the type, and a
// type mismatch should surface as the handler's own invalid_args, not be
// silently reinterpreted.)
inline std::optional<bool> arg_bool(const Request& req,
                                     std::string_view key) {
    const mcp::JsonValue* n = arg_node(req, key);
    if (n == nullptr || !n->is_bool()) {
        return std::nullopt;
    }
    return n->as_bool();
}

}  // namespace remote_hands::wire

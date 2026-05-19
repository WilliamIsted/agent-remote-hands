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

// SchemaArgs — the Phase-2.1 named-argument resolution pattern, promoted to
// shared infrastructure for the ~11 namespaces migrating off positional
// parsing. The window.* namespace was the pattern-setter; this header is the
// single definition every other namespace's verbs read through.
//
// Each verb constructs a SchemaArgs naming its input_schema properties IN
// SCHEMA ORDER. A property's value is resolved as:
//
//   1. named:      req.named_root["<prop>"]    (the MCP arguments object)
//   2. positional: req.named_root["_args"][i]  (i = the prop's schema index)
//
// (1) is the real MCP surface an LLM client produces from input_schema.
// (2) is the v2.2 reference client's projection of a *positional* call
// (`client.request("window.move", h, "0", "0")` -> {"_args":[h,"0","0"]});
// resolving it by schema slot keeps input_schema the single source of truth
// for both the key names AND their order.
//
// Typed reads are string-tolerant: the reference client sends `--key value`
// as JSON strings (e.g. {"pid":"1234"}, {"visible_only":"false"}) and bare
// `--flag` as a real JSON bool. A real LLM client sends schema-typed JSON.
// Accepting both is the strangler-fig contract during the migration and
// mirrors how map_arguments() coerces the positional path today.

#include "../connection.hpp"
#include "../errors.hpp"
#include "../json.hpp"
#include "args.hpp"

#include <charconv>
#include <climits>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace remote_hands::wire {

class SchemaArgs {
public:
    SchemaArgs(const wire::Request& req,
               std::initializer_list<std::string_view> props)
        : req_(req), props_(props) {
        const mcp::JsonValue* root = wire::named_args(req);
        if (root != nullptr) {
            const mcp::JsonValue* a = root->find("_args");
            if (a != nullptr && a->is_array()) positional_ = a;
        }
    }

    // The JSON node for a schema property, resolved named-first then by the
    // property's schema-order positional slot. nullptr/JSON-null => absent.
    const mcp::JsonValue* node(std::string_view prop) const {
        if (const mcp::JsonValue* n = wire::arg_node(req_, prop)) {
            if (!n->is_null()) return n;
        }
        if (positional_ != nullptr) {
            const auto idx = index_of(prop);
            if (idx >= 0) {
                const auto& arr = positional_->as_array();
                if (static_cast<std::size_t>(idx) < arr.size()) {
                    const mcp::JsonValue* p = &arr[static_cast<std::size_t>(idx)];
                    if (!p->is_null()) return p;
                }
            }
        }
        return nullptr;
    }

    bool present(std::string_view prop) const { return node(prop) != nullptr; }

    // String value. Present-as-string -> the string; present-as-number ->
    // its exact lexeme (handles still arrive as strings, but be liberal).
    //
    // str() is scalar-only — it returns std::nullopt for JSON object/array
    // nodes by design; structured arguments (e.g. region {x,y,w,h},
    // modifiers [...]) must be read via node() and walked explicitly.
    std::optional<std::string> str(std::string_view prop) const {
        const mcp::JsonValue* n = node(prop);
        if (n == nullptr) return std::nullopt;
        if (n->is_string()) return n->as_string();
        if (n->is_number()) return n->num_lexeme();
        if (n->is_bool())   return n->as_bool() ? std::string("true")
                                                : std::string("false");
        return std::nullopt;
    }

    // Integer value, string-tolerant (reference client sends `--x 10` as the
    // string "10"). Reads through the exact lexeme so no double rounding.
    std::optional<long long> integer(std::string_view prop) const {
        const mcp::JsonValue* n = node(prop);
        if (n == nullptr) return std::nullopt;
        if (n->is_number()) {
            return parse_ll(n->num_lexeme());
        }
        if (n->is_string()) {
            return parse_ll(n->as_string());
        }
        return std::nullopt;
    }

    // Like integer() but constrained to 32-bit int range. Returns nullopt if
    // absent, non-numeric, OR outside [INT_MIN, INT_MAX] — so a hostile
    // out-of-range value surfaces as the handler's own invalid_args rather than
    // a silent narrowing wrap. Width-bounded handler args (coordinates, sizes,
    // counts that feed Win32 ints) MUST use this, not static_cast<int>(integer()).
    std::optional<int> integer32(std::string_view prop) const {
        const auto v = integer(prop);
        if (!v) return std::nullopt;
        if (*v < INT_MIN || *v > INT_MAX) return std::nullopt;
        return static_cast<int>(*v);
    }

    // Boolean value, string-tolerant ("true"/"false" from `--flag value`;
    // real JSON bool from a bare `--flag` or a typed client).
    std::optional<bool> boolean(std::string_view prop) const {
        const mcp::JsonValue* n = node(prop);
        if (n == nullptr) return std::nullopt;
        if (n->is_bool()) return n->as_bool();
        if (n->is_string()) {
            const std::string& s = n->as_string();
            if (s == "true"  || s == "1") return true;
            if (s == "false" || s == "0") return false;
        }
        return std::nullopt;
    }

private:
    int index_of(std::string_view prop) const {
        for (std::size_t i = 0; i < props_.size(); ++i) {
            if (props_[i] == prop) return static_cast<int>(i);
        }
        return -1;
    }

    static std::optional<long long> parse_ll(std::string_view s) {
        // Tolerate a leading minus and surrounding nothing else. Reject a
        // value that does not parse cleanly so a bad arg surfaces as the
        // handler's own invalid_args rather than a silent 0.
        //
        // std::from_chars parses a leading '-' for signed targets natively
        // and handles INT64_MIN correctly, so the sign is NOT stripped
        // manually (manual strip + negate overflows on -9223372036854775808
        // because 9223372036854775808 cannot be held in a signed long long).
        // from_chars does NOT accept a leading '+', so reject it explicitly
        // to preserve the prior "reject junk" behaviour. The parse must
        // consume the entire string ("12x"/""/"+5" => nullopt).
        if (s.empty()) return std::nullopt;
        if (s.front() == '+') return std::nullopt;
        long long v = 0;
        const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
        if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
        return v;
    }

    const wire::Request& req_;
    std::vector<std::string_view> props_;
    const mcp::JsonValue* positional_ = nullptr;
};

// Emit ERR invalid_args with the same {"message":...} shape the positional
// handlers produced, so error ergonomics are unchanged by the migration.
inline void invalid_args(Connection& conn, std::string_view message) {
    std::string detail = "{";
    json::append_kv_string(detail, "message", message);
    detail += '}';
    conn.writer().write_err(ErrorCode::InvalidArgs, detail);
}

}  // namespace remote_hands::wire

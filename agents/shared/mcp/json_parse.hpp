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

// Minimal recursive-descent JSON parser for inbound MCP JSON-RPC frames.
//
// The agent's json.hpp is a *builder* only — it produces JSON, it cannot
// consume it. MCP framing (§1.6) requires parsing inbound JSON-RPC requests
// (`tools/call` arguments, `initialize` params, …) so this header introduces
// a small, correct, dependency-free parser kept entirely separate from the
// builder so json.hpp stays "build-only".
//
// Scope: RFC 8259 — objects, arrays, strings (with \uXXXX surrogate pairs and
// the standard escapes), numbers (int + float, exponent), true/false/null,
// arbitrary nesting. Numbers are retained both as a double and as their exact
// source lexeme so integer round-trips ("42", not "42.000000") survive — the
// tools/call arg mapper renders numbers back to wire tokens verbatim.
//
// On any malformed input parse() returns std::nullopt; callers map that to an
// MCP JSON-RPC protocol-level error (-32700 parse error).

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace remote_hands::mcp {

class JsonValue;
using JsonArray  = std::vector<JsonValue>;
// Object preserves insertion order so re-emitted argument iteration is stable.
using JsonObject = std::vector<std::pair<std::string, JsonValue>>;

enum class JsonType { Null, Bool, Number, String, Array, Object };

class JsonValue {
public:
    JsonValue() : type_(JsonType::Null) {}

    static JsonValue make_null()                 { return JsonValue(); }
    static JsonValue make_bool(bool b)           { JsonValue v; v.type_ = JsonType::Bool;   v.bool_ = b; return v; }
    static JsonValue make_number(double d, std::string lex) {
        JsonValue v; v.type_ = JsonType::Number; v.num_ = d; v.num_lexeme_ = std::move(lex); return v;
    }
    static JsonValue make_string(std::string s) { JsonValue v; v.type_ = JsonType::String; v.str_ = std::move(s); return v; }
    static JsonValue make_array(JsonArray a)    { JsonValue v; v.type_ = JsonType::Array;  v.arr_ = std::move(a); return v; }
    static JsonValue make_object(JsonObject o)  { JsonValue v; v.type_ = JsonType::Object; v.obj_ = std::move(o); return v; }

    JsonType type() const noexcept { return type_; }
    bool is_null()   const noexcept { return type_ == JsonType::Null; }
    bool is_bool()   const noexcept { return type_ == JsonType::Bool; }
    bool is_number() const noexcept { return type_ == JsonType::Number; }
    bool is_string() const noexcept { return type_ == JsonType::String; }
    bool is_array()  const noexcept { return type_ == JsonType::Array; }
    bool is_object() const noexcept { return type_ == JsonType::Object; }

    bool               as_bool()   const noexcept { return bool_; }
    double             as_number() const noexcept { return num_; }
    const std::string& num_lexeme() const noexcept { return num_lexeme_; }
    const std::string& as_string() const noexcept { return str_; }
    const JsonArray&   as_array()  const noexcept { return arr_; }
    const JsonObject&  as_object() const noexcept { return obj_; }

    // Object lookup. Returns nullptr if not an object or key absent.
    const JsonValue* find(std::string_view key) const noexcept {
        if (type_ != JsonType::Object) return nullptr;
        for (const auto& kv : obj_) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }

private:
    JsonType    type_;
    bool        bool_ = false;
    double      num_  = 0.0;
    std::string num_lexeme_;
    std::string str_;
    JsonArray   arr_;
    JsonObject  obj_;
};

// Parse a complete JSON document. Trailing non-whitespace is rejected.
// Returns nullopt on any malformed input.
std::optional<JsonValue> parse(std::string_view text);

}  // namespace remote_hands::mcp

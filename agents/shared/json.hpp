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

// Minimal JSON-string builder helpers used by verb handlers.
//
// Builds short JSON bodies into a std::string accumulator. The caller controls
// placement of `{` `}` `[` `]` and commas. We avoid a full JSON dependency
// because every body the agent produces is small, fixed-shape, and built once
// per verb invocation; pulling in a parser/serialiser would dwarf the rest of
// the binary.
//
// Header-only on purpose so each translation unit gets its own inline copy.

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace remote_hands::json {

// Appends a JSON-quoted string with standard escaping (RFC 8259).
inline void append_string(std::string& out, std::string_view s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char esc[8];
                    std::snprintf(esc, sizeof(esc), "\\u%04x",
                                  static_cast<unsigned>(c));
                    out += esc;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

inline void append_kv_string(std::string& out, std::string_view key,
                             std::string_view value) {
    append_string(out, key);
    out += ':';
    append_string(out, value);
}

inline void append_kv_int(std::string& out, std::string_view key, long long v) {
    append_string(out, key);
    char buf[32];
    std::snprintf(buf, sizeof(buf), ":%lld", v);
    out += buf;
}

inline void append_kv_uint(std::string& out, std::string_view key,
                           unsigned long long v) {
    append_string(out, key);
    char buf[32];
    std::snprintf(buf, sizeof(buf), ":%llu", v);
    out += buf;
}

inline void append_kv_bool(std::string& out, std::string_view key, bool v) {
    append_string(out, key);
    out += v ? ":true" : ":false";
}

inline void append_kv_null(std::string& out, std::string_view key) {
    append_string(out, key);
    out += ":null";
}

inline void append_string_array(std::string& out, std::string_view key,
                                const std::vector<std::string>& items) {
    append_string(out, key);
    out += ":[";
    bool first = true;
    for (const auto& s : items) {
        if (!first) out += ',';
        first = false;
        append_string(out, s);
    }
    out += ']';
}

}  // namespace remote_hands::json

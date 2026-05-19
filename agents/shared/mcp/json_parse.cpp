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

#include "json_parse.hpp"

#include <cstdlib>
#include <cstring>

namespace remote_hands::mcp {

namespace {

// Recursive-descent parser over a string_view. Depth-bounded to defend
// against pathological deeply-nested adversarial input on the (unauthenticated)
// wire.
constexpr int kMaxDepth = 64;

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    bool parse_document(JsonValue& out) {
        skip_ws();
        if (!parse_value(out, 0)) return false;
        skip_ws();
        return pos_ == s_.size();  // reject trailing garbage
    }

private:
    std::string_view s_;
    std::size_t      pos_ = 0;

    bool eof() const { return pos_ >= s_.size(); }
    char peek() const { return s_[pos_]; }

    void skip_ws() {
        while (!eof()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool match_literal(std::string_view lit) {
        if (s_.size() - pos_ < lit.size()) return false;
        if (s_.compare(pos_, lit.size(), lit) != 0) return false;
        pos_ += lit.size();
        return true;
    }

    bool parse_value(JsonValue& out, int depth) {
        if (depth > kMaxDepth) return false;
        skip_ws();
        if (eof()) return false;
        const char c = peek();
        switch (c) {
            case '{': return parse_object(out, depth);
            case '[': return parse_array(out, depth);
            case '"': {
                std::string str;
                if (!parse_string(str)) return false;
                out = JsonValue::make_string(std::move(str));
                return true;
            }
            case 't':
                if (!match_literal("true")) return false;
                out = JsonValue::make_bool(true);
                return true;
            case 'f':
                if (!match_literal("false")) return false;
                out = JsonValue::make_bool(false);
                return true;
            case 'n':
                if (!match_literal("null")) return false;
                out = JsonValue::make_null();
                return true;
            default:
                if (c == '-' || (c >= '0' && c <= '9')) {
                    return parse_number(out);
                }
                return false;
        }
    }

    bool parse_object(JsonValue& out, int depth) {
        ++pos_;  // consume '{'
        JsonObject obj;
        skip_ws();
        if (!eof() && peek() == '}') {
            ++pos_;
            out = JsonValue::make_object(std::move(obj));
            return true;
        }
        while (true) {
            skip_ws();
            if (eof() || peek() != '"') return false;
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (eof() || peek() != ':') return false;
            ++pos_;  // consume ':'
            JsonValue val;
            if (!parse_value(val, depth + 1)) return false;
            obj.emplace_back(std::move(key), std::move(val));
            skip_ws();
            if (eof()) return false;
            const char c = peek();
            if (c == ',') { ++pos_; continue; }
            if (c == '}') { ++pos_; break; }
            return false;
        }
        out = JsonValue::make_object(std::move(obj));
        return true;
    }

    bool parse_array(JsonValue& out, int depth) {
        ++pos_;  // consume '['
        JsonArray arr;
        skip_ws();
        if (!eof() && peek() == ']') {
            ++pos_;
            out = JsonValue::make_array(std::move(arr));
            return true;
        }
        while (true) {
            JsonValue val;
            if (!parse_value(val, depth + 1)) return false;
            arr.push_back(std::move(val));
            skip_ws();
            if (eof()) return false;
            const char c = peek();
            if (c == ',') { ++pos_; continue; }
            if (c == ']') { ++pos_; break; }
            return false;
        }
        out = JsonValue::make_array(std::move(arr));
        return true;
    }

    // Appends a UTF-8 encoding of a Unicode code point to `out`.
    static void append_utf8(std::string& out, unsigned int cp) {
        if (cp <= 0x7F) {
            out += static_cast<char>(cp);
        } else if (cp <= 0x7FF) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool parse_hex4(unsigned int& out) {
        if (s_.size() - pos_ < 4) return false;
        unsigned int v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9')      v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        out = v;
        return true;
    }

    bool parse_string(std::string& out) {
        if (eof() || peek() != '"') return false;
        ++pos_;  // consume opening quote
        out.clear();
        while (true) {
            if (eof()) return false;
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (eof()) return false;
                const char e = s_[pos_++];
                switch (e) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        unsigned int cp = 0;
                        if (!parse_hex4(cp)) return false;
                        // Surrogate pair handling.
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (s_.size() - pos_ < 2 ||
                                s_[pos_] != '\\' || s_[pos_ + 1] != 'u') {
                                return false;
                            }
                            pos_ += 2;
                            unsigned int lo = 0;
                            if (!parse_hex4(lo)) return false;
                            if (lo < 0xDC00 || lo > 0xDFFF) return false;
                            cp = 0x10000 +
                                 ((cp - 0xD800) << 10) +
                                 (lo - 0xDC00);
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            // Unpaired low surrogate.
                            return false;
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default:
                        return false;
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                // Raw control characters are invalid inside JSON strings.
                return false;
            } else {
                out += c;
            }
        }
    }

    bool parse_number(JsonValue& out) {
        const std::size_t start = pos_;
        if (!eof() && peek() == '-') ++pos_;
        if (eof()) return false;
        if (peek() == '0') {
            ++pos_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (!eof() && peek() >= '0' && peek() <= '9') ++pos_;
        } else {
            return false;
        }
        if (!eof() && peek() == '.') {
            ++pos_;
            if (eof() || peek() < '0' || peek() > '9') return false;
            while (!eof() && peek() >= '0' && peek() <= '9') ++pos_;
        }
        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            ++pos_;
            if (!eof() && (peek() == '+' || peek() == '-')) ++pos_;
            if (eof() || peek() < '0' || peek() > '9') return false;
            while (!eof() && peek() >= '0' && peek() <= '9') ++pos_;
        }
        const std::string lexeme(s_.substr(start, pos_ - start));
        // std::strtod over a NUL-terminated copy (string_view is not).
        const double d = std::strtod(lexeme.c_str(), nullptr);
        out = JsonValue::make_number(d, lexeme);
        return true;
    }
};

}  // namespace

std::optional<JsonValue> parse(std::string_view text) {
    Parser p(text);
    JsonValue v;
    if (!p.parse_document(v)) return std::nullopt;
    return v;
}

}  // namespace remote_hands::mcp

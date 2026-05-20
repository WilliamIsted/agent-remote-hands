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

// `vision.*` namespace verb handler.
//
// Implements vision.ocr — text extraction via the platform OCR seam.
// Five mutually exclusive source selectors: region, window, monitor, path, bytes.
// Delegates pixel/byte acquisition to screen::capture_* then to platform::ocr_*.
//
// R6: also implements vision.describe — captures region/window/monitor/full
// screen, encodes PNG, POSTs to an OpenAI-compatible /v1/chat/completions
// endpoint with the screenshot embedded as an image_url data URL, returns
// the assistant message text + usage stats. WinHTTP is the HTTP client
// (already linked via file.cpp for file.download); the response is parsed
// with the agent's existing mcp::parse / mcp::JsonValue facility.

#include "../base64.hpp"
#include "../capabilities.hpp"
#include "../connection.hpp"
#include "../errors.hpp"
#include "../image_encode.hpp"
#include "../json.hpp"
#include "../log.hpp"
#include "../mcp/json_parse.hpp"
#include "../platform.hpp"
#include "../screen_capture.hpp"
#include "../text_util.hpp"
#include "args.hpp"
#include "schema_args.hpp"

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>   // vision.describe — HTTP client (same lib file.cpp uses)

#pragma comment(lib, "winhttp.lib")

namespace remote_hands::vision_verbs {

using wire::SchemaArgs;
using wire::invalid_args;

namespace {

// ---------------------------------------------------------------------------
// Helpers shared with screen.cpp (duplicated per D3 — no cross-verb headers)

HWND parse_hwnd(std::string_view s) {
    if (s.size() < 5 || s.substr(0, 4) != "win:") return nullptr;
    s.remove_prefix(4);
    if (s.size() >= 2 && (s.substr(0, 2) == "0x" || s.substr(0, 2) == "0X"))
        s.remove_prefix(2);
    if (s.empty()) return nullptr;
    unsigned long long v = 0;
    const auto* end = s.data() + s.size();
    const auto [p, ec] = std::from_chars(s.data(), end, v, 16);
    if (ec != std::errc{} || p != end) return nullptr;
    return reinterpret_cast<HWND>(static_cast<uintptr_t>(v));
}

bool parse_float(std::string_view s, float& out) {
    // from_chars for float is C++17 but may not be available on all MSVC versions;
    // fall back to sscanf which is always safe here.
    char buf[64];
    if (s.size() >= sizeof(buf)) return false;
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    float v = 0.0f;
    char* endp = nullptr;
    // Use strtof for portable float parse.
    v = std::strtof(buf, &endp);
    if (endp == buf || *endp != '\0') return false;
    out = v;
    return true;
}

struct MonitorByIndex {
    int target_index  = 0;
    int current_index = 0;
    RECT rect{};
    bool found = false;
};

BOOL CALLBACK find_monitor_by_index(HMONITOR mon, HDC, LPRECT, LPARAM lparam) {
    auto* state = reinterpret_cast<MonitorByIndex*>(lparam);
    if (state->current_index == state->target_index) {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(mon, &info)) {
            state->rect  = info.rcMonitor;
            state->found = true;
        }
        return FALSE;
    }
    state->current_index++;
    return TRUE;
}

// ---------------------------------------------------------------------------
// Utilities specific to vision.ocr

// Decode standard base64 into bytes. Skips whitespace; stops at '='.
std::vector<uint8_t> base64_decode(std::string_view s) {
    auto char_val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve((s.size() * 3) / 4);
    int val = 0, bits = 0;
    for (char c : s) {
        if (c == '=') break;
        int v = char_val(c);
        if (v < 0) continue;
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>(val >> bits));
            val &= (1 << bits) - 1;
        }
    }
    return out;
}

// Read a file (UTF-8 path) into a byte vector. Returns false on error.
bool read_file_bytes(const std::string& path_utf8, std::vector<uint8_t>& out) {
    const int wlen = MultiByteToWideChar(CP_UTF8, 0,
                                         path_utf8.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring wpath(static_cast<std::size_t>(wlen - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path_utf8.c_str(), -1, wpath.data(), wlen);

    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 ||
        sz.QuadPart > 256LL * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<std::size_t>(sz.QuadPart));
    DWORD nread = 0;
    const bool ok =
        ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &nread, nullptr)
        && nread == static_cast<DWORD>(out.size());
    CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}

// ---------------------------------------------------------------------------
// JSON emission helpers

void append_bbox(std::string& j, int x, int y, int w, int h) {
    json::append_string(j, "bbox");
    j += ":{";
    json::append_kv_int(j, "x", x); j += ',';
    json::append_kv_int(j, "y", y); j += ',';
    json::append_kv_int(j, "w", w); j += ',';
    json::append_kv_int(j, "h", h);
    j += '}';
}

void append_kv_float(std::string& j, std::string_view key, float v) {
    json::append_string(j, key);
    char buf[32];
    std::snprintf(buf, sizeof(buf), ":%g", static_cast<double>(v));
    j += buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// vision.ocr handler

// vision.ocr — input_schema (schema order):
//   ["region","window","monitor","path","bytes","bytes_format",
//    "language","min_confidence","include_word_bboxes"]
// x-mutually-exclusive: ["region","window","monitor","path","bytes"].
// x-conditional: bytes present => bytes_format present (invalid_args).
// x-errors: ["not_supported","not_found","permission_denied",
//            "unsupported_format","image_too_large","invalid_args"].
void ocr(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"region", "window", "monitor", "path", "bytes",
                          "bytes_format", "language", "min_confidence",
                          "include_word_bboxes"});
    if (args.reject_unknown(conn)) return;

    bool has_region  = false, has_window  = false, has_monitor = false;
    bool has_path    = false, has_bytes   = false;
    bool include_word_bboxes = false;
    int  rx = 0, ry = 0, rw = 0, rh = 0;
    int  monitor_index = 0;
    HWND hwnd = nullptr;
    std::string path_str, bytes_b64, bytes_format_str, language_hint;
    float min_confidence = 0.0f;

    // --- region: nested object {x,y,w,h}; w,h minimum 1 -------------------
    if (args.present("region")) {
        const mcp::JsonValue* rnode = args.node("region");
        if (rnode == nullptr || !rnode->is_object()) {
            invalid_args(conn,
                         "vision.ocr 'region' must be an object {x,y,w,h}");
            return;
        }
        struct Member { const char* key; int* out; bool min_one; };
        const Member members[] = {
            {"x", &rx, false}, {"y", &ry, false},
            {"w", &rw, true},  {"h", &rh, true},
        };
        for (const Member& m : members) {
            const mcp::JsonValue* v = rnode->find(m.key);
            if (v == nullptr || v->is_null()) {
                invalid_args(conn, "vision.ocr 'region' requires x,y,w,h");
                return;
            }
            std::optional<std::string> lex;
            if (v->is_number())      lex = v->num_lexeme();
            else if (v->is_string()) lex = v->as_string();
            if (!lex) {
                invalid_args(conn,
                             "vision.ocr 'region' x,y,w,h must be integers");
                return;
            }
            const char* begin = lex->data();
            const char* end   = begin + lex->size();
            if (begin != end && *begin == '+') {
                invalid_args(conn,
                             "vision.ocr 'region' x,y,w,h must be integers");
                return;
            }
            long long parsed = 0;
            const auto [p, ec] = std::from_chars(begin, end, parsed, 10);
            if (ec != std::errc{} || p != end ||
                parsed < INT_MIN || parsed > INT_MAX) {
                invalid_args(conn,
                             "vision.ocr 'region' x,y,w,h must be integers");
                return;
            }
            if (m.min_one && parsed < 1) {
                invalid_args(conn,
                             "vision.ocr 'region' w and h must be >= 1");
                return;
            }
            *m.out = static_cast<int>(parsed);
        }
        has_region = true;
    }

    // --- window: string handle "win:0x<hex>" ------------------------------
    if (args.present("window")) {
        auto w = args.str("window");
        if (!w) {
            invalid_args(conn,
                         "vision.ocr 'window' must be a string (win:0x<hex>)");
            return;
        }
        hwnd = parse_hwnd(*w);
        if (!hwnd) {
            invalid_args(conn,
                         "vision.ocr 'window' must be win:0x<hex>");
            return;
        }
        has_window = true;
    }

    // --- monitor: zero-based integer index --------------------------------
    if (args.present("monitor")) {
        auto m = args.integer32("monitor");
        if (!m || *m < 0) {
            invalid_args(conn,
                         "vision.ocr 'monitor' must be a non-negative "
                         "integer");
            return;
        }
        monitor_index = *m;
        has_monitor = true;
    }

    // --- path: absolute image-file path -----------------------------------
    if (args.present("path")) {
        auto p = args.str("path");
        if (!p || p->empty()) {
            invalid_args(conn, "vision.ocr 'path' must be a non-empty string");
            return;
        }
        path_str = std::move(*p);
        has_path = true;
    }

    // --- bytes: base64 image buffer ---------------------------------------
    if (args.present("bytes")) {
        auto b = args.str("bytes");
        if (!b) {
            invalid_args(conn, "vision.ocr 'bytes' must be a base64 string");
            return;
        }
        bytes_b64 = std::move(*b);
        has_bytes = true;
    }

    // --- bytes_format: codec of `bytes` (required iff bytes present) ------
    if (args.present("bytes_format")) {
        auto f = args.str("bytes_format");
        if (!f) {
            invalid_args(conn, "vision.ocr 'bytes_format' must be a string");
            return;
        }
        bytes_format_str = std::move(*f);
    }

    // --- language: BCP-47 tag ---------------------------------------------
    if (args.present("language")) {
        auto l = args.str("language");
        if (!l) {
            invalid_args(conn, "vision.ocr 'language' must be a string");
            return;
        }
        language_hint = std::move(*l);
    }

    // --- min_confidence: number 0.0-1.0, default 0.0 ----------------------
    if (args.present("min_confidence")) {
        auto c = args.str("min_confidence");
        if (!c || !parse_float(*c, min_confidence) ||
            min_confidence < 0.0f || min_confidence > 1.0f) {
            invalid_args(conn,
                         "vision.ocr 'min_confidence' must be a number "
                         "0.0-1.0");
            return;
        }
    }

    // --- include_word_bboxes: boolean, default false ----------------------
    if (args.present("include_word_bboxes")) {
        auto b = args.boolean("include_word_bboxes");
        if (!b) {
            invalid_args(conn,
                         "vision.ocr 'include_word_bboxes' must be a "
                         "boolean");
            return;
        }
        include_word_bboxes = *b;
    }

    // x-mutually-exclusive: region / window / monitor / path / bytes.
    const int selector_count = (has_region  ? 1 : 0) + (has_window ? 1 : 0)
                             + (has_monitor ? 1 : 0) + (has_path   ? 1 : 0)
                             + (has_bytes   ? 1 : 0);
    if (selector_count > 1) {
        invalid_args(conn,
                     "vision.ocr source selectors are mutually exclusive");
        return;
    }
    // x-conditional: bytes_format required when bytes is supplied.
    if (has_bytes && bytes_format_str.empty()) {
        invalid_args(conn,
                     "vision.ocr 'bytes_format' is required when 'bytes' "
                     "is supplied");
        return;
    }

    platform::OcrRecognition result;
    int img_x = 0, img_y = 0, img_w = 0, img_h = 0;
    std::string coordinate_space;

    const auto& caps = platform::ocr_capabilities();

    try {
        if (has_window) {
            screen::CapturedFrame frame = screen::capture_window(hwnd, false);
            if (frame.width == 0 || frame.height == 0) {
                conn.writer().write_err(ErrorCode::NotFound,
                    "{\"message\":\"window capture failed\"}");
                return;
            }
            if (frame.width > caps.max_dimension || frame.height > caps.max_dimension) {
                char detail[128];
                std::snprintf(detail, sizeof(detail),
                    "{\"reason\":\"image_too_large\",\"max_dimension\":%d,"
                    "\"observed\":{\"w\":%d,\"h\":%d}}",
                    caps.max_dimension, frame.width, frame.height);
                conn.writer().write_err(ErrorCode::ImageTooLarge, detail);
                return;
            }
            result = platform::ocr_from_frame(frame, 0, 0, language_hint,
                                              min_confidence, include_word_bboxes);
            coordinate_space = "screen";
            img_x = 0; img_y = 0;
            img_w = result.image_width; img_h = result.image_height;

        } else if (has_region) {
            if (rw <= 0 || rh <= 0) {
                conn.writer().write_err(ErrorCode::InvalidArgs,
                    "{\"message\":\"'region' w and h must be positive\"}");
                return;
            }
            screen::CapturedFrame frame = screen::capture_region(rx, ry, rw, rh, false);
            if (frame.width > caps.max_dimension || frame.height > caps.max_dimension) {
                char detail[128];
                std::snprintf(detail, sizeof(detail),
                    "{\"reason\":\"image_too_large\",\"max_dimension\":%d,"
                    "\"observed\":{\"w\":%d,\"h\":%d}}",
                    caps.max_dimension, frame.width, frame.height);
                conn.writer().write_err(ErrorCode::ImageTooLarge, detail);
                return;
            }
            result = platform::ocr_from_frame(frame, rx, ry, language_hint,
                                              min_confidence, include_word_bboxes);
            coordinate_space = "screen";
            img_x = rx; img_y = ry;
            img_w = result.image_width; img_h = result.image_height;

        } else if (has_monitor) {
            MonitorByIndex state{};
            state.target_index = monitor_index;
            EnumDisplayMonitors(nullptr, nullptr, find_monitor_by_index,
                                reinterpret_cast<LPARAM>(&state));
            if (!state.found) {
                char detail[80];
                std::snprintf(detail, sizeof(detail),
                              "{\"message\":\"monitor index %d not present\"}",
                              monitor_index);
                conn.writer().write_err(ErrorCode::NotFound, detail);
                return;
            }
            const int mx = state.rect.left;
            const int my = state.rect.top;
            const int mw = state.rect.right  - state.rect.left;
            const int mh = state.rect.bottom - state.rect.top;
            screen::CapturedFrame frame = screen::capture_region(mx, my, mw, mh, false);
            if (frame.width > caps.max_dimension || frame.height > caps.max_dimension) {
                char detail[128];
                std::snprintf(detail, sizeof(detail),
                    "{\"reason\":\"image_too_large\",\"max_dimension\":%d,"
                    "\"observed\":{\"w\":%d,\"h\":%d}}",
                    caps.max_dimension, frame.width, frame.height);
                conn.writer().write_err(ErrorCode::ImageTooLarge, detail);
                return;
            }
            result = platform::ocr_from_frame(frame, mx, my, language_hint,
                                              min_confidence, include_word_bboxes);
            coordinate_space = "screen";
            img_x = mx; img_y = my;
            img_w = result.image_width; img_h = result.image_height;

        } else if (has_path) {
            std::vector<uint8_t> file_bytes;
            if (!read_file_bytes(path_str, file_bytes)) {
                conn.writer().write_err(ErrorCode::NotFound,
                    "{\"message\":\"file not found or unreadable\"}");
                return;
            }
            result = platform::ocr_from_bytes(file_bytes.data(), file_bytes.size(),
                                              "" /* sniff from magic bytes */,
                                              language_hint, min_confidence,
                                              include_word_bboxes);
            coordinate_space = "image";
            img_x = 0; img_y = 0;
            img_w = result.image_width; img_h = result.image_height;

        } else if (has_bytes) {
            auto decoded = base64_decode(bytes_b64);
            if (decoded.empty()) {
                conn.writer().write_err(ErrorCode::InvalidArgs,
                    "{\"message\":\"'bytes': base64 decode produced empty buffer\"}");
                return;
            }
            result = platform::ocr_from_bytes(decoded.data(), decoded.size(),
                                              bytes_format_str, language_hint,
                                              min_confidence, include_word_bboxes);
            coordinate_space = "image";
            img_x = 0; img_y = 0;
            img_w = result.image_width; img_h = result.image_height;

        } else {
            // No selector: capture the full virtual screen.
            screen::CapturedFrame frame = screen::capture_virtual_screen(false);
            if (frame.width > caps.max_dimension || frame.height > caps.max_dimension) {
                char detail[128];
                std::snprintf(detail, sizeof(detail),
                    "{\"reason\":\"image_too_large\",\"max_dimension\":%d,"
                    "\"observed\":{\"w\":%d,\"h\":%d}}",
                    caps.max_dimension, frame.width, frame.height);
                conn.writer().write_err(ErrorCode::ImageTooLarge, detail);
                return;
            }
            result = platform::ocr_from_frame(frame, 0, 0, language_hint,
                                              min_confidence, include_word_bboxes);
            coordinate_space = "screen";
            img_x = 0; img_y = 0;
            img_w = result.image_width; img_h = result.image_height;
        }

    } catch (const std::runtime_error& e) {
        const std::string_view msg = e.what();
        if (msg == "not_supported") {
            conn.writer().write_err(ErrorCode::NotSupported,
                "{\"reason\":\"no OCR language pack installed\"}");
        } else if (msg == "unsupported_format") {
            conn.writer().write_err(ErrorCode::UnsupportedFormat,
                "{\"reason\":\"unsupported_format\"}");
        } else if (msg == "image_too_large") {
            char detail[80];
            std::snprintf(detail, sizeof(detail),
                "{\"reason\":\"image_too_large\",\"max_dimension\":%d}",
                caps.max_dimension);
            conn.writer().write_err(ErrorCode::ImageTooLarge, detail);
        } else {
            conn.writer().write_err(ErrorCode::NotSupported,
                "{\"reason\":\"engine_error\"}");
        }
        return;
    }

    // -----------------------------------------------------------------------
    // Build the JSON response.

    // Full text: lines joined by \n (already filtered by map_result).
    std::string full_text;
    for (auto const& line : result.lines) {
        if (!full_text.empty()) full_text += '\n';
        full_text += line.text;
    }

    std::string j;
    j.reserve(1024);
    j += '{';

    json::append_kv_string(j, "text", full_text);
    j += ',';

    // lines array
    json::append_string(j, "lines");
    j += ":[";
    bool first_line = true;
    for (auto const& line : result.lines) {
        if (!first_line) j += ',';
        first_line = false;
        j += '{';
        json::append_kv_string(j, "text", line.text);
        j += ',';
        append_bbox(j, line.x, line.y, line.w, line.h);
        if (line.has_confidence) {
            j += ',';
            append_kv_float(j, "confidence", line.confidence);
        }
        if (!line.words.empty()) {
            j += ',';
            json::append_string(j, "words");
            j += ":[";
            bool first_word = true;
            for (auto const& word : line.words) {
                if (!first_word) j += ',';
                first_word = false;
                j += '{';
                json::append_kv_string(j, "text", word.text);
                j += ',';
                append_bbox(j, word.x, word.y, word.w, word.h);
                j += '}';
            }
            j += ']';
        }
        j += '}';
    }
    j += ']';
    j += ',';

    json::append_kv_string(j, "language_used", result.language_used);
    j += ',';
    json::append_kv_string(j, "coordinate_space", coordinate_space);
    j += ',';

    // image_size
    json::append_string(j, "image_size");
    j += ":{";
    json::append_kv_int(j, "x", img_x); j += ',';
    json::append_kv_int(j, "y", img_y); j += ',';
    json::append_kv_int(j, "w", img_w); j += ',';
    json::append_kv_int(j, "h", img_h);
    j += '}';
    j += ',';

    append_kv_float(j, "text_angle", result.text_angle);

    j += '}';
    conn.writer().write_ok(j);
}

// ---------------------------------------------------------------------------
// vision.describe handler (R6)
//
// Pipeline:
//   1. Resolve source selector (region/window/monitor/[default full screen])
//      — exactly one may be set; mutually exclusive.
//   2. Resolve endpoint (per-call > CLI/env default; missing -> invalid_args
//      with reason:"vision_endpoint_missing").
//   3. Capture frame via screen::capture_*  (same helpers vision.ocr uses).
//   4. PNG-encode the frame via image::encode_png (WIC).
//   5. Build the OpenAI-compatible request body (model + messages + max_tokens
//      + temperature; image embedded as data:image/png;base64,<b64>).
//   6. POST via WinHTTP with timeout_ms applied via WinHttpSetTimeouts.
//   7. Parse the response body with mcp::parse, extract
//      choices[0].message.content, response.model, optional
//      usage.{prompt_tokens,completion_tokens}.
//   8. Emit the spec OK shape:
//        {description, model, [tokens_in], [tokens_out], elapsed_ms}
//
// All WinHTTP handle paths close on every error and on success — same lifecycle
// discipline as file.download.

namespace {

// Default prompt — matches the R6 task brief verbatim.
constexpr const char* kDefaultPrompt =
    "Describe what is visible on this screen, including UI elements and any "
    "visible text. Be concise.";

constexpr int kDefaultMaxTokens   =   512;
constexpr int kMinMaxTokens       =    16;
constexpr int kMaxMaxTokens       =  4096;
constexpr int kDefaultTimeoutMs   = 30000;
constexpr int kMinTimeoutMs       =  1000;
constexpr int kMaxTimeoutMs       =300000;

// Build the OpenAI-compatible chat/completions request body. `model` is
// omitted when empty (server picks the loaded model — matches LM Studio's
// "model:''" convention).
//
// Wire shape (locked by the R6 brief):
//   {"model":"...",                       // omitted if model empty
//    "messages":[{"role":"user","content":[
//        {"type":"text","text":"<prompt>"},
//        {"type":"image_url","image_url":{"url":"data:image/png;base64,<b64>"}}
//    ]}],
//    "max_tokens":<n>,
//    "temperature":0.2}
//
// json::append_string handles every JSON escape required for the prompt
// (quotes, backslashes, control bytes, RFC 8259 \uXXXX form). The image
// base64 payload is RFC 4648 standard alphabet -> no JSON escaping needed
// but it goes through append_string anyway for uniformity.
std::string build_openai_request(std::string_view model,
                                 std::string_view prompt,
                                 std::string_view image_b64,
                                 int max_tokens) {
    std::string body;
    body.reserve(image_b64.size() + prompt.size() + 256);
    body += '{';

    if (!model.empty()) {
        json::append_kv_string(body, "model", model);
        body += ',';
    }

    json::append_string(body, "messages");
    body += ":[{";
    json::append_kv_string(body, "role", "user");
    body += ',';
    json::append_string(body, "content");
    body += ":[";

    // text content item
    body += '{';
    json::append_kv_string(body, "type", "text");
    body += ',';
    json::append_kv_string(body, "text", prompt);
    body += '}';

    body += ',';

    // image_url content item
    body += '{';
    json::append_kv_string(body, "type", "image_url");
    body += ',';
    json::append_string(body, "image_url");
    body += ":{";
    // Construct the data URL prefix; append_string handles all escaping.
    std::string data_url;
    data_url.reserve(image_b64.size() + 32);
    data_url += "data:image/png;base64,";
    data_url.append(image_b64);
    json::append_kv_string(body, "url", data_url);
    body += '}';
    body += '}';

    body += "]}],";

    json::append_kv_int(body, "max_tokens", max_tokens);
    body += ',';

    // temperature: 0.2 — descriptions, not creativity. (json.hpp has no
    // append_kv_double helper; hand-format with two decimal places.)
    json::append_string(body, "temperature");
    body += ":0.2";

    body += '}';
    return body;
}

// HTTP POST helper. Returns true on success and fills response_body. On
// failure writes the spec ERR and returns false (caller just returns).
//
// Error mapping (matches R6 x-errors):
//   * WinHttpCrackUrl / scheme reject  -> invalid_args
//   * WinHttpOpen failure              -> not_supported  (session init)
//   * Send/Receive timeout             -> timeout
//   * Send/Receive non-timeout         -> transfer_failed
//   * Non-2xx HTTP status              -> transfer_failed
//
// Every successfully-opened WinHTTP handle is released on every path
// (success AND every error). Mirrors file.download's close_all() discipline.
bool http_post_json(Connection& conn,
                    const std::string& endpoint_utf8,
                    const std::string& request_body,
                    int timeout_ms,
                    std::string& response_body) {
    response_body.clear();

    const std::wstring wurl = text::utf8_to_wide(endpoint_utf8);
    URL_COMPONENTS uc{};
    uc.dwStructSize       = sizeof(uc);
    wchar_t host[256]     = {0};
    wchar_t urlpath[2048] = {0};
    wchar_t scheme[16]    = {0};
    uc.lpszHostName       = host;     uc.dwHostNameLength = 255;
    uc.lpszUrlPath        = urlpath;  uc.dwUrlPathLength  = 2047;
    uc.lpszScheme         = scheme;   uc.dwSchemeLength   = 15;
    if (!WinHttpCrackUrl(wurl.c_str(),
                         static_cast<DWORD>(wurl.size()), 0, &uc)) {
        invalid_args(conn,
                     "vision.describe 'endpoint' is not a valid absolute "
                     "http/https URL");
        return false;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTP &&
        uc.nScheme != INTERNET_SCHEME_HTTPS) {
        invalid_args(conn,
                     "vision.describe 'endpoint' scheme must be http or https");
        return false;
    }
    const bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET hSession = WinHttpOpen(
        L"agent-remote-hands/0.3 (vision.describe; WinHTTP)",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        char d[80];
        std::snprintf(d, sizeof(d),
                      "{\"reason\":\"winhttp_open_failed\",\"win32_error\":%lu}",
                      GetLastError());
        conn.writer().write_err(ErrorCode::NotSupported, d);
        return false;
    }

    // Apply the verb's timeout to every WinHTTP phase. The four parameters
    // are: resolve, connect, send, receive. We use the same value for all
    // four — timeout_ms is the verb's spec timeout for the whole HTTP call.
    WinHttpSetTimeouts(hSession, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) {
        char d[80];
        std::snprintf(d, sizeof(d),
                      "{\"reason\":\"winhttp_connect_failed\","
                      "\"win32_error\":%lu}", GetLastError());
        WinHttpCloseHandle(hSession);
        conn.writer().write_err(ErrorCode::TransferFailed, d);
        return false;
    }

    DWORD req_flags = https ? WINHTTP_FLAG_SECURE : 0u;
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"POST", urlpath, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, req_flags);
    if (!hRequest) {
        char d[80];
        std::snprintf(d, sizeof(d),
                      "{\"reason\":\"winhttp_open_request_failed\","
                      "\"win32_error\":%lu}", GetLastError());
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        conn.writer().write_err(ErrorCode::TransferFailed, d);
        return false;
    }

    auto close_all = [&]() {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
    };

    static const wchar_t kHeaders[] = L"Content-Type: application/json\r\n";
    const DWORD body_len = static_cast<DWORD>(request_body.size());
    if (!WinHttpSendRequest(hRequest, kHeaders,
                            static_cast<DWORD>(wcslen(kHeaders)),
                            // body buffer can be passed in dwTotalLength + a
                            // single WinHttpWriteData call; pass the whole
                            // thing here directly via SendRequest's optional
                            // lpOptional/dwOptionalLength.
                            const_cast<char*>(request_body.data()), body_len,
                            body_len, 0) ||
        !WinHttpReceiveResponse(hRequest, nullptr)) {
        const DWORD err = GetLastError();
        close_all();
        // Timeout codes: ERROR_WINHTTP_TIMEOUT  (12002) and the related
        // operation-cancelled-by-timeout flavours surface as TIMEOUT;
        // unreachable / DNS / TLS failures fall under transfer_failed.
        if (err == ERROR_WINHTTP_TIMEOUT) {
            char d[80];
            std::snprintf(d, sizeof(d),
                "{\"reason\":\"http_timeout\",\"win32_error\":%lu}", err);
            conn.writer().write_err(ErrorCode::Timeout, d);
            return false;
        }
        char d[96];
        std::snprintf(d, sizeof(d),
                      "{\"reason\":\"http_send_failed\",\"win32_error\":%lu}",
                      err);
        conn.writer().write_err(ErrorCode::TransferFailed, d);
        return false;
    }

    // HTTP status code. WinHttpQueryHeaders can fail on a malformed response;
    // if it does, status would remain 0 and the non-2xx branch below would
    // misreport status_code:0 instead of distinguishing the parse failure
    // from a server status. Surface the query failure as its own error so
    // the caller doesn't get a misleading status_code:0.
    DWORD status = 0, slen = sizeof(status);
    if (!WinHttpQueryHeaders(
            hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen,
            WINHTTP_NO_HEADER_INDEX)) {
        const DWORD err = GetLastError();
        close_all();
        char d[96];
        std::snprintf(d, sizeof(d),
                      "{\"reason\":\"http_query_status_failed\","
                      "\"win32_error\":%lu}",
                      err);
        conn.writer().write_err(ErrorCode::TransferFailed, d);
        return false;
    }
    if (status < 200 || status >= 300) {
        // Drain a small slice of the body so the caller's detail can include
        // it (best-effort — cap at 1 KiB so a giant error page can't blow up
        // the ERR frame). OpenAI-compatible servers return a JSON error here;
        // we forward the raw bytes so the caller can inspect.
        std::string partial;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
            if (partial.size() >= 1024) break;
            const DWORD want = static_cast<DWORD>(
                std::min<std::size_t>(avail, 1024 - partial.size()));
            std::vector<char> chunk(want);
            DWORD got = 0;
            if (!WinHttpReadData(hRequest, chunk.data(), want, &got) ||
                got == 0) break;
            partial.append(chunk.data(), got);
        }
        close_all();
        std::string detail = "{";
        json::append_kv_string(detail, "reason", "http_status");
        detail += ',';
        json::append_kv_int(detail, "status_code",
                            static_cast<long long>(status));
        if (!partial.empty()) {
            detail += ',';
            json::append_kv_string(detail, "body_excerpt", partial);
        }
        detail += '}';
        conn.writer().write_err(ErrorCode::TransferFailed, detail);
        return false;
    }

    // Read the response body.
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) {
            const DWORD err = GetLastError();
            close_all();
            char d[80];
            std::snprintf(d, sizeof(d),
                "{\"reason\":\"http_read_failed\",\"win32_error\":%lu}", err);
            conn.writer().write_err(ErrorCode::TransferFailed, d);
            return false;
        }
        if (avail == 0) break;
        // Bound the total response size — a runaway server shouldn't be able
        // to exhaust agent memory. 4 MiB is comfortably larger than any
        // realistic chat-completions JSON for a description-class verb.
        if (response_body.size() + avail > 4u * 1024u * 1024u) {
            close_all();
            conn.writer().write_err(ErrorCode::TransferFailed,
                "{\"reason\":\"response_too_large\","
                "\"message\":\"vision.describe response exceeded 4 MiB\"}");
            return false;
        }
        std::vector<char> chunk(avail);
        DWORD got = 0;
        if (!WinHttpReadData(hRequest, chunk.data(), avail, &got)) {
            const DWORD err = GetLastError();
            close_all();
            char d[80];
            std::snprintf(d, sizeof(d),
                "{\"reason\":\"http_read_failed\",\"win32_error\":%lu}", err);
            conn.writer().write_err(ErrorCode::TransferFailed, d);
            return false;
        }
        if (got == 0) break;
        response_body.append(chunk.data(), got);
    }

    close_all();
    return true;
}

// Pull an integer out of a usage.* member if present. JsonValue keeps both
// the double and the source lexeme; parse the lexeme so an integer stays an
// integer on the wire.
bool extract_usage_int(const mcp::JsonValue* usage_node,
                       std::string_view key,
                       long long& out) {
    if (usage_node == nullptr || !usage_node->is_object()) return false;
    const mcp::JsonValue* n = usage_node->find(key);
    if (n == nullptr || !n->is_number()) return false;
    const std::string& lex = n->num_lexeme();
    long long v = 0;
    auto* begin = lex.data();
    auto* end   = lex.data() + lex.size();
    auto [p, ec] = std::from_chars(begin, end, v, 10);
    if (ec != std::errc{}) {
        // Fractional usage values are nonsensical from the OpenAI shape; if
        // we ever see one, truncate via the double round-trip rather than
        // refuse.
        out = static_cast<long long>(n->as_number());
        return true;
    }
    (void)p;
    out = v;
    return true;
}

}  // namespace

// vision.describe — input_schema (schema order):
//   ["region","window","monitor","endpoint","model","prompt","max_tokens",
//    "timeout_ms"]
// x-mutually-exclusive: ["region","window","monitor"].
// x-errors: ["not_found","invalid_args","not_supported","transfer_failed",
//            "timeout","permission_denied"].
// x-output-schema:
//   {description: string, model: string, tokens_in?: int, tokens_out?: int,
//    elapsed_ms: int}
void describe(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"region", "window", "monitor",
                          "endpoint", "model", "prompt",
                          "max_tokens", "timeout_ms"});
    if (args.reject_unknown(conn)) return;

    bool has_region  = false, has_window = false, has_monitor = false;
    int  rx = 0, ry = 0, rw = 0, rh = 0;
    int  monitor_index = 0;
    HWND hwnd = nullptr;
    std::string endpoint_arg, model_str;
    std::string prompt_str = kDefaultPrompt;
    int max_tokens = kDefaultMaxTokens;
    int timeout_ms = kDefaultTimeoutMs;

    // --- region: nested object {x,y,w,h}; w,h minimum 1 -------------------
    if (args.present("region")) {
        const mcp::JsonValue* rnode = args.node("region");
        if (rnode == nullptr || !rnode->is_object()) {
            invalid_args(conn,
                         "vision.describe 'region' must be an object {x,y,w,h}");
            return;
        }
        struct Member { const char* key; int* out; bool min_one; };
        const Member members[] = {
            {"x", &rx, false}, {"y", &ry, false},
            {"w", &rw, true},  {"h", &rh, true},
        };
        for (const Member& m : members) {
            const mcp::JsonValue* v = rnode->find(m.key);
            if (v == nullptr || v->is_null()) {
                invalid_args(conn,
                             "vision.describe 'region' requires x,y,w,h");
                return;
            }
            std::optional<std::string> lex;
            if (v->is_number())      lex = v->num_lexeme();
            else if (v->is_string()) lex = v->as_string();
            if (!lex) {
                invalid_args(conn,
                    "vision.describe 'region' x,y,w,h must be integers");
                return;
            }
            const char* begin = lex->data();
            const char* end   = begin + lex->size();
            if (begin != end && *begin == '+') {
                invalid_args(conn,
                    "vision.describe 'region' x,y,w,h must be integers");
                return;
            }
            long long parsed = 0;
            const auto [p, ec] = std::from_chars(begin, end, parsed, 10);
            if (ec != std::errc{} || p != end ||
                parsed < INT_MIN || parsed > INT_MAX) {
                invalid_args(conn,
                    "vision.describe 'region' x,y,w,h must be integers");
                return;
            }
            if (m.min_one && parsed < 1) {
                invalid_args(conn,
                    "vision.describe 'region' w and h must be >= 1");
                return;
            }
            *m.out = static_cast<int>(parsed);
        }
        has_region = true;
    }

    // --- window: string handle "win:0x<hex>" ------------------------------
    if (args.present("window")) {
        auto w = args.str("window");
        if (!w) {
            invalid_args(conn,
                "vision.describe 'window' must be a string (win:0x<hex>)");
            return;
        }
        hwnd = parse_hwnd(*w);
        if (!hwnd) {
            invalid_args(conn,
                "vision.describe 'window' must be win:0x<hex>");
            return;
        }
        has_window = true;
    }

    // --- monitor: zero-based integer index --------------------------------
    if (args.present("monitor")) {
        auto m = args.integer32("monitor");
        if (!m || *m < 0) {
            invalid_args(conn,
                "vision.describe 'monitor' must be a non-negative integer");
            return;
        }
        monitor_index = *m;
        has_monitor = true;
    }

    // x-mutually-exclusive: region / window / monitor.
    const int selector_count = (has_region ? 1 : 0)
                             + (has_window ? 1 : 0)
                             + (has_monitor ? 1 : 0);
    if (selector_count > 1) {
        invalid_args(conn,
            "vision.describe source selectors region/window/monitor are "
            "mutually exclusive");
        return;
    }

    // --- endpoint ---------------------------------------------------------
    if (args.present("endpoint")) {
        auto e = args.str("endpoint");
        if (!e || e->empty()) {
            invalid_args(conn,
                "vision.describe 'endpoint' must be a non-empty string");
            return;
        }
        endpoint_arg = std::move(*e);
    }

    // --- model (optional, may be empty -> server picks default) -----------
    if (args.present("model")) {
        auto m = args.str("model");
        if (!m) {
            invalid_args(conn,
                "vision.describe 'model' must be a string");
            return;
        }
        model_str = std::move(*m);
    }

    // --- prompt (optional, default kDefaultPrompt) ------------------------
    if (args.present("prompt")) {
        auto p = args.str("prompt");
        if (!p) {
            invalid_args(conn,
                "vision.describe 'prompt' must be a string");
            return;
        }
        prompt_str = std::move(*p);
    }

    // --- max_tokens (optional, default 512, range 16..4096) ---------------
    if (args.present("max_tokens")) {
        auto v = args.integer32("max_tokens");
        if (!v || *v < kMinMaxTokens || *v > kMaxMaxTokens) {
            invalid_args(conn,
                "vision.describe 'max_tokens' must be an integer 16..4096");
            return;
        }
        max_tokens = *v;
    }

    // --- timeout_ms (optional, default 30000, range 1000..300000) ---------
    if (args.present("timeout_ms")) {
        auto v = args.integer32("timeout_ms");
        if (!v || *v < kMinTimeoutMs || *v > kMaxTimeoutMs) {
            invalid_args(conn,
                "vision.describe 'timeout_ms' must be an integer "
                "1000..300000");
            return;
        }
        timeout_ms = *v;
    }

    // --- resolve endpoint (per-call > CLI/env default > error) ------------
    std::string endpoint_resolved =
        endpoint_arg.empty() ? remote_hands::vision_endpoint() : endpoint_arg;
    if (endpoint_resolved.empty()) {
        std::string detail = "{";
        json::append_kv_string(detail, "reason", "vision_endpoint_missing");
        detail += ',';
        json::append_kv_string(detail, "message",
            "vision.describe requires endpoint arg or --vision-endpoint "
            "CLI default");
        detail += '}';
        conn.writer().write_err(ErrorCode::InvalidArgs, detail);
        return;
    }

    // --- capture frame ----------------------------------------------------
    screen::CapturedFrame frame;
    if (has_window) {
        frame = screen::capture_window(hwnd, false);
        if (frame.width == 0 || frame.height == 0) {
            conn.writer().write_err(ErrorCode::NotFound,
                "{\"message\":\"window capture failed\"}");
            return;
        }
    } else if (has_region) {
        if (rw <= 0 || rh <= 0) {
            invalid_args(conn,
                "vision.describe 'region' w and h must be positive");
            return;
        }
        frame = screen::capture_region(rx, ry, rw, rh, false);
        if (frame.width == 0 || frame.height == 0) {
            conn.writer().write_err(ErrorCode::NotFound,
                "{\"message\":\"region capture failed\"}");
            return;
        }
    } else if (has_monitor) {
        MonitorByIndex state{};
        state.target_index = monitor_index;
        EnumDisplayMonitors(nullptr, nullptr, find_monitor_by_index,
                            reinterpret_cast<LPARAM>(&state));
        if (!state.found) {
            char detail[80];
            std::snprintf(detail, sizeof(detail),
                "{\"message\":\"monitor index %d not present\"}",
                monitor_index);
            conn.writer().write_err(ErrorCode::NotFound, detail);
            return;
        }
        const int mx = state.rect.left;
        const int my = state.rect.top;
        const int mw = state.rect.right  - state.rect.left;
        const int mh = state.rect.bottom - state.rect.top;
        frame = screen::capture_region(mx, my, mw, mh, false);
        if (frame.width == 0 || frame.height == 0) {
            conn.writer().write_err(ErrorCode::NotFound,
                "{\"message\":\"monitor capture failed\"}");
            return;
        }
    } else {
        // No selector -> full virtual screen.
        frame = screen::capture_virtual_screen(false);
        if (frame.width == 0 || frame.height == 0) {
            conn.writer().write_err(ErrorCode::NotSupported,
                "{\"reason\":\"capture_failed\","
                "\"message\":\"virtual-screen capture returned empty frame\"}");
            return;
        }
    }

    // --- PNG-encode (WIC; modern-only is fine — verb gated to Modern) -----
    std::vector<std::byte> png_bytes = image::encode_png(frame);
    if (png_bytes.empty()) {
        conn.writer().write_err(ErrorCode::NotSupported,
            "{\"reason\":\"image_encode_failed\","
            "\"message\":\"PNG encode returned empty buffer\"}");
        return;
    }

    // --- base64 + build request body --------------------------------------
    const std::string image_b64 =
        remote_hands::base64_encode(png_bytes.data(), png_bytes.size());
    const std::string request_body = build_openai_request(
        model_str, prompt_str, image_b64, max_tokens);

    // --- POST (timed) -----------------------------------------------------
    const auto t0 = std::chrono::steady_clock::now();
    std::string response_body;
    if (!http_post_json(conn, endpoint_resolved, request_body, timeout_ms,
                        response_body)) {
        return;   // http_post_json already wrote the ERR frame.
    }
    const auto t1 = std::chrono::steady_clock::now();
    const long long elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    // --- parse response ---------------------------------------------------
    auto parsed = mcp::parse(response_body);
    if (!parsed || !parsed->is_object()) {
        conn.writer().write_err(ErrorCode::TransferFailed,
            "{\"reason\":\"response_not_json\","
            "\"message\":\"vision.describe response was not parseable JSON\"}");
        return;
    }

    // choices[0].message.content (string)
    const mcp::JsonValue* choices = parsed->find("choices");
    if (choices == nullptr || !choices->is_array() ||
        choices->as_array().empty()) {
        conn.writer().write_err(ErrorCode::TransferFailed,
            "{\"reason\":\"response_missing_choices\","
            "\"message\":\"vision.describe response missing choices[0]\"}");
        return;
    }
    const mcp::JsonValue& choice0 = choices->as_array().front();
    const mcp::JsonValue* msg = choice0.find("message");
    if (msg == nullptr || !msg->is_object()) {
        conn.writer().write_err(ErrorCode::TransferFailed,
            "{\"reason\":\"response_missing_message\","
            "\"message\":\"vision.describe response missing "
            "choices[0].message\"}");
        return;
    }
    const mcp::JsonValue* content = msg->find("content");
    if (content == nullptr || !content->is_string()) {
        conn.writer().write_err(ErrorCode::TransferFailed,
            "{\"reason\":\"response_missing_content\","
            "\"message\":\"vision.describe response missing "
            "choices[0].message.content (string)\"}");
        return;
    }
    const std::string& description = content->as_string();

    // model echo (response.model, may be absent on some servers — emit "" if so)
    std::string model_used;
    if (const mcp::JsonValue* mm = parsed->find("model"); mm && mm->is_string()) {
        model_used = mm->as_string();
    }

    // optional usage.{prompt_tokens,completion_tokens}
    const mcp::JsonValue* usage = parsed->find("usage");
    long long tokens_in = 0, tokens_out = 0;
    const bool has_tokens_in  = extract_usage_int(usage, "prompt_tokens",
                                                  tokens_in);
    const bool has_tokens_out = extract_usage_int(usage, "completion_tokens",
                                                  tokens_out);

    // --- emit OK body -----------------------------------------------------
    std::string out;
    out.reserve(description.size() + 256);
    out += '{';
    json::append_kv_string(out, "description", description);
    out += ',';
    json::append_kv_string(out, "model", model_used);
    if (has_tokens_in) {
        out += ',';
        json::append_kv_int(out, "tokens_in", tokens_in);
    }
    if (has_tokens_out) {
        out += ',';
        json::append_kv_int(out, "tokens_out", tokens_out);
    }
    out += ',';
    json::append_kv_int(out, "elapsed_ms", elapsed_ms);
    out += '}';
    conn.writer().write_ok(out);
}

}  // namespace remote_hands::vision_verbs

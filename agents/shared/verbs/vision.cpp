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

#include "../capabilities.hpp"
#include "../connection.hpp"
#include "../errors.hpp"
#include "../json.hpp"
#include "../log.hpp"
#include "../platform.hpp"
#include "../screen_capture.hpp"
#include "args.hpp"
#include "schema_args.hpp"

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

}  // namespace remote_hands::vision_verbs

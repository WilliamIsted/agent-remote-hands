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

// `screen.*` namespace verb handler.
//
// Implements PROTOCOL.md §4.2:
//   screen.capture [--region x,y,w,h] [--window <hwnd>] [--format <fmt>]
//
// Capture path: BitBlt today (universal). WGC is a future runtime-detected
// fast path documented in COMPATIBILITY.md.
//
// Encoder: PNG (default, via WIC) and BMP. WebP is deferred until libwebp
// is bundled — `system.info.capabilities.image_formats` advertises only the
// formats actually available.

#include "../connection.hpp"
#include "../errors.hpp"
#include "../image_encode.hpp"
#include "../json.hpp"
#include "../log.hpp"
#include "../screen_capture.hpp"

#include <charconv>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace remote_hands::screen_verbs {

namespace {

enum class Format { Png, Bmp };

bool parse_format(std::string_view s, Format& out) {
    if (s == "png" || s.empty()) { out = Format::Png; return true; }
    if (s == "bmp")               { out = Format::Bmp; return true; }
    // "webp" / "webp:NN" advertised only when libwebp lands.
    return false;
}

bool parse_region(std::string_view s, int& x, int& y, int& w, int& h) {
    int values[4] = {};
    std::size_t cursor = 0;
    for (int i = 0; i < 4; ++i) {
        const std::size_t comma = s.find(',', cursor);
        const std::size_t end_pos = (comma == std::string_view::npos) ? s.size() : comma;
        if (end_pos == cursor) return false;

        const auto* p_end = s.data() + end_pos;
        long parsed = 0;
        const auto [p, ec] = std::from_chars(s.data() + cursor, p_end, parsed, 10);
        if (ec != std::errc{} || p != p_end) return false;

        values[i] = static_cast<int>(parsed);
        if (comma == std::string_view::npos && i < 3) return false;
        cursor = end_pos + 1;
    }
    x = values[0]; y = values[1]; w = values[2]; h = values[3];
    return true;
}

HWND parse_hwnd(std::string_view s) {
    if (s.size() < 5 || s.substr(0, 4) != "win:") return nullptr;
    s.remove_prefix(4);
    if (s.size() >= 2 && (s.substr(0, 2) == "0x" || s.substr(0, 2) == "0X")) {
        s.remove_prefix(2);
    }
    if (s.empty()) return nullptr;
    unsigned long long v = 0;
    const auto* end = s.data() + s.size();
    const auto [p, ec] = std::from_chars(s.data(), end, v, 16);
    if (ec != std::errc{} || p != end) return nullptr;
    return reinterpret_cast<HWND>(static_cast<uintptr_t>(v));
}

bool parse_nonneg_int(std::string_view s, int& out) {
    int v = 0;
    const auto* end = s.data() + s.size();
    const auto [p, ec] = std::from_chars(s.data(), end, v, 10);
    if (ec != std::errc{} || p != end || v < 0) return false;
    out = v;
    return true;
}

// EnumDisplayMonitors callback that selects the Nth monitor (0-based),
// matching the same enumeration order as window.list's monitor_index.
struct MonitorByIndex {
    int target_index = 0;
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
            state->rect = info.rcMonitor;
            state->found = true;
        }
        return FALSE;
    }
    state->current_index++;
    return TRUE;
}

}  // namespace

void capture(Connection& conn, const wire::Request& req) {
    Format format = Format::Png;
    bool   has_region  = false;
    bool   has_window  = false;
    bool   has_monitor = false;
    int    rx = 0, ry = 0, rw = 0, rh = 0;
    int    monitor_index = 0;
    HWND   hwnd = nullptr;

    for (std::size_t i = 0; i < req.args.size(); ++i) {
        if (req.args[i] == "--region" && i + 1 < req.args.size()) {
            if (!parse_region(req.args[++i], rx, ry, rw, rh)) {
                conn.writer().write_err(
                    ErrorCode::InvalidArgs,
                    "{\"message\":\"--region must be x,y,w,h\"}");
                return;
            }
            has_region = true;
        } else if (req.args[i] == "--window" && i + 1 < req.args.size()) {
            hwnd = parse_hwnd(req.args[++i]);
            if (!hwnd) {
                conn.writer().write_err(
                    ErrorCode::InvalidArgs,
                    "{\"message\":\"--window must be win:0x<hex>\"}");
                return;
            }
            has_window = true;
        } else if (req.args[i] == "--monitor" && i + 1 < req.args.size()) {
            if (!parse_nonneg_int(req.args[++i], monitor_index)) {
                conn.writer().write_err(
                    ErrorCode::InvalidArgs,
                    "{\"message\":\"--monitor must be a non-negative integer\"}");
                return;
            }
            has_monitor = true;
        } else if (req.args[i] == "--format" && i + 1 < req.args.size()) {
            if (!parse_format(req.args[++i], format)) {
                conn.writer().write_err(
                    ErrorCode::NotSupported,
                    "{\"reason\":\"unsupported format; advertised in system.info.capabilities.image_formats\"}");
                return;
            }
        }
    }

    const int region_likes = (has_region ? 1 : 0) + (has_window ? 1 : 0) + (has_monitor ? 1 : 0);
    if (region_likes > 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"--region / --window / --monitor are mutually exclusive\"}");
        return;
    }

    screen::CapturedFrame frame;
    if (has_window) {
        frame = screen::capture_window(hwnd);
    } else if (has_region) {
        frame = screen::capture_region(rx, ry, rw, rh);
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
        frame = screen::capture_region(state.rect.left, state.rect.top,
                                       state.rect.right - state.rect.left,
                                       state.rect.bottom - state.rect.top);
    } else {
        frame = screen::capture_virtual_screen();
    }

    if (frame.width == 0 || frame.height == 0 || frame.pixels.empty()) {
        conn.writer().write_err(
            ErrorCode::NotSupported,
            "{\"reason\":\"capture produced empty frame\"}");
        return;
    }

    std::vector<std::byte> encoded =
        (format == Format::Png) ? image::encode_png(frame)
                                : image::encode_bmp(frame);

    if (encoded.empty()) {
        conn.writer().write_err(
            ErrorCode::NotSupported,
            "{\"reason\":\"encoder failed\"}");
        return;
    }

    conn.writer().write_ok(std::span<const std::byte>(encoded));
}

}  // namespace remote_hands::screen_verbs

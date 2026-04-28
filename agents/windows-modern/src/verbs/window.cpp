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

// `window.*` namespace verb handlers.
//
// Implements the verbs in PROTOCOL.md §4.3:
//   window.list                  (observe)
//   window.find                  (observe)
//   window.focus                 (drive)
//   window.close                 (drive)
//   window.move                  (drive)
//   window.state                 (observe)
//
// Window handles are advertised on the wire as `win:0x<hex>`. Helpers in the
// anonymous namespace below convert between HWND and the wire form. When
// input.* lands in build phase 8, these helpers move to a shared header.

#include "../connection.hpp"
#include "../errors.hpp"
#include "../json.hpp"
#include "../log.hpp"

#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace remote_hands::window_verbs {

namespace {

// ---------------------------------------------------------------------------
// HWND <-> wire-string helpers

std::string hwnd_to_string(HWND hwnd) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "win:0x%llx",
                  static_cast<unsigned long long>(
                      reinterpret_cast<uintptr_t>(hwnd)));
    return buf;
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

// ---------------------------------------------------------------------------
// Window inspection

DWORD get_window_pid(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid;
}

std::string get_window_title_utf8(HWND hwnd) {
    wchar_t buf[512];
    const int len = GetWindowTextW(hwnd, buf, static_cast<int>(std::size(buf)));
    if (len <= 0) return {};

    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, buf, len, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, len, out.data(), needed, nullptr, nullptr);
    return out;
}

// "Phantom" windows are visible-but-not-meaningful: zero geometry, off-monitor,
// empty title, or marked WS_EX_NOACTIVATE / WS_EX_TOOLWINDOW. Filtered out of
// window.list by default; --all bypasses.
bool is_phantom_window(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) return true;

    const LONG_PTR exstyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exstyle & WS_EX_NOACTIVATE) return true;
    if (exstyle & WS_EX_TOOLWINDOW) return true;

    RECT r;
    if (!GetWindowRect(hwnd, &r)) return true;
    if (r.right - r.left <= 0 || r.bottom - r.top <= 0) return true;

    // Windows parks minimised / off-screen windows at coords like (-32000,
    // -32000) where they intersect no monitor. MONITOR_DEFAULTTONULL returns
    // null in that case.
    if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) == nullptr) return true;

    if (GetWindowTextLengthW(hwnd) == 0) return true;

    return false;
}

bool starts_with_ci(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

void append_window_object(std::string& out, HWND hwnd) {
    RECT r{};
    GetWindowRect(hwnd, &r);

    out += '{';
    json::append_kv_string(out, "hwnd", hwnd_to_string(hwnd));         out += ',';
    json::append_kv_int(out, "x", r.left);                             out += ',';
    json::append_kv_int(out, "y", r.top);                              out += ',';
    json::append_kv_int(out, "w", r.right - r.left);                   out += ',';
    json::append_kv_int(out, "h", r.bottom - r.top);                   out += ',';
    json::append_kv_string(out, "title", get_window_title_utf8(hwnd)); out += ',';
    json::append_kv_uint(out, "pid", get_window_pid(hwnd));
    out += '}';
}

std::string_view show_cmd_to_state(UINT cmd) {
    switch (cmd) {
        case SW_HIDE:               return "hidden";
        case SW_SHOWMINIMIZED:
        case SW_MINIMIZE:
        case SW_SHOWMINNOACTIVE:    return "minimised";
        case SW_SHOWMAXIMIZED:      return "maximised";
        default:                    return "normal";
    }
}

// Common pre-call validation for verbs that operate on a target hwnd. Returns
// the parsed HWND, or nullptr after writing an error response.
HWND require_target(Connection& conn, std::string_view raw) {
    HWND target = parse_hwnd(raw);
    if (!target || !IsWindow(target)) {
        std::string detail = "{";
        json::append_kv_string(detail, "handle", raw);
        detail += '}';
        conn.writer().write_err(ErrorCode::TargetGone, detail);
        return nullptr;
    }
    return target;
}

// ---------------------------------------------------------------------------
// EnumWindows callbacks

struct ListContext {
    bool        include_all = false;
    std::string filter_prefix;
    std::string out;
    bool        first = true;
};

BOOL CALLBACK enum_for_list(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<ListContext*>(lparam);

    if (!ctx->include_all && is_phantom_window(hwnd)) return TRUE;

    if (!ctx->filter_prefix.empty()) {
        if (!starts_with_ci(get_window_title_utf8(hwnd), ctx->filter_prefix)) {
            return TRUE;
        }
    }

    if (!ctx->first) ctx->out += ',';
    ctx->first = false;
    append_window_object(ctx->out, hwnd);

    return TRUE;
}

struct FindContext {
    std::string prefix;
    HWND        result = nullptr;
};

BOOL CALLBACK enum_for_find(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<FindContext*>(lparam);

    if (is_phantom_window(hwnd)) return TRUE;
    if (!starts_with_ci(get_window_title_utf8(hwnd), ctx->prefix)) return TRUE;

    ctx->result = hwnd;
    return FALSE;  // stop enumeration
}

}  // namespace

// ---------------------------------------------------------------------------
// window.list

void list(Connection& conn, const wire::Request& req) {
    ListContext ctx;

    for (std::size_t i = 0; i < req.args.size(); ++i) {
        if (req.args[i] == "--all") {
            ctx.include_all = true;
        } else if (req.args[i] == "--filter" && i + 1 < req.args.size()) {
            ctx.filter_prefix = req.args[++i];
        }
    }

    ctx.out = "{\"windows\":[";
    EnumWindows(enum_for_list, reinterpret_cast<LPARAM>(&ctx));
    ctx.out += "]}";
    conn.writer().write_ok(ctx.out);
}

// ---------------------------------------------------------------------------
// window.find

void find(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"window.find requires <title-pattern>\"}");
        return;
    }

    FindContext ctx;
    ctx.prefix = req.args[0];
    EnumWindows(enum_for_find, reinterpret_cast<LPARAM>(&ctx));

    if (ctx.result == nullptr) {
        conn.writer().write_err(ErrorCode::NotFound);
        return;
    }

    std::string out;
    append_window_object(out, ctx.result);
    conn.writer().write_ok(out);
}

// ---------------------------------------------------------------------------
// window.focus

void focus(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"window.focus requires <hwnd>\"}");
        return;
    }

    HWND target = require_target(conn, req.args[0]);
    if (!target) return;

    HWND prior = GetForegroundWindow();

    // Grant ourselves the right to set the foreground window. This won't
    // bypass every lock case (the OS reserves that), but it handles the
    // common case of an idle desktop.
    AllowSetForegroundWindow(ASFW_ANY);

    if (!SetForegroundWindow(target) || GetForegroundWindow() != target) {
        conn.writer().write_err(
            ErrorCode::LockHeld,
            "{\"lock_type\":\"foreground\"}");
        return;
    }

    // Record the focused target so subsequent input verbs on this connection
    // can short-circuit with ERR target_gone if the foreground switches away
    // (see crash_check.hpp / closes #46).
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    conn.note_focus_target(target, pid);

    std::string body = "{";
    json::append_kv_string(body, "prior_hwnd", hwnd_to_string(prior));
    body += '}';
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// window.close

void close(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"window.close requires <hwnd>\"}");
        return;
    }

    HWND target = require_target(conn, req.args[0]);
    if (!target) return;

    // PostMessage rather than SendMessage so we don't block on an unresponsive
    // target. The window handles WM_CLOSE on its own message pump and may
    // refuse (e.g. unsaved-document prompt).
    PostMessageW(target, WM_CLOSE, 0, 0);
    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// window.move

void move(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 5) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"window.move requires <hwnd> <x> <y> <w> <h>\"}");
        return;
    }

    HWND target = require_target(conn, req.args[0]);
    if (!target) return;

    const int x = std::atoi(req.args[1].c_str());
    const int y = std::atoi(req.args[2].c_str());
    const int w = std::atoi(req.args[3].c_str());
    const int h = std::atoi(req.args[4].c_str());

    if (!SetWindowPos(target, nullptr, x, y, w, h,
                      SWP_NOZORDER | SWP_NOACTIVATE)) {
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", GetLastError());
        conn.writer().write_err(ErrorCode::NotSupported, detail);
        return;
    }

    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// window.state

void state(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"window.state requires <hwnd>\"}");
        return;
    }

    HWND target = require_target(conn, req.args[0]);
    if (!target) return;

    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(target, &wp)) {
        conn.writer().write_err(ErrorCode::NotSupported);
        return;
    }

    std::string body = "{";
    json::append_kv_string(body, "state", show_cmd_to_state(wp.showCmd));
    body += '}';
    conn.writer().write_ok(body);
}

}  // namespace remote_hands::window_verbs

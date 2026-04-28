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

// `input.*` namespace verb handlers.
//
// Implements the verbs in PROTOCOL.md §4.4:
//   input.click          (drive)
//   input.move           (drive)
//   input.scroll         (drive)
//   input.key            (drive)
//   input.type           (drive)  -- length-prefixed UTF-8 payload
//   input.send_message   (drive)
//
// Every verb (except input.move and input.send_message which target a known
// hwnd or no hwnd at all) checks the foreground window's integrity level
// before injecting; if the IL barrier blocks input, the verb returns
// `ERR uipi_blocked` with `{agent_il, target_il}` rather than silently
// no-op'ing as v1 did.

#include "../connection.hpp"
#include "../crash_check.hpp"
#include "../errors.hpp"
#include "../json.hpp"
#include "../log.hpp"
#include "../uipi.hpp"

#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace remote_hands::input_verbs {

namespace {

// ---------------------------------------------------------------------------
// HWND parsing (duplicated from window.cpp; will move to a shared header
// when a third namespace needs it).

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
// Key-name → virtual-key code mapping

struct NamedKey {
    const char* name;
    WORD        vk;
};

constexpr NamedKey kNamedKeys[] = {
    {"enter",       VK_RETURN},
    {"return",      VK_RETURN},
    {"tab",         VK_TAB},
    {"esc",         VK_ESCAPE},
    {"escape",      VK_ESCAPE},
    {"space",       VK_SPACE},
    {"backspace",   VK_BACK},
    {"back",        VK_BACK},
    {"delete",      VK_DELETE},
    {"del",         VK_DELETE},
    {"insert",      VK_INSERT},
    {"ins",         VK_INSERT},
    {"home",        VK_HOME},
    {"end",         VK_END},
    {"pgup",        VK_PRIOR},
    {"pgdn",        VK_NEXT},
    {"up",          VK_UP},
    {"down",        VK_DOWN},
    {"left",        VK_LEFT},
    {"right",       VK_RIGHT},
    {"win",         VK_LWIN},
    {"ctrl",        VK_CONTROL},
    {"control",     VK_CONTROL},
    {"alt",         VK_MENU},
    {"shift",       VK_SHIFT},
    {"capslock",    VK_CAPITAL},
    {"caps",        VK_CAPITAL},
    {"numlock",     VK_NUMLOCK},
    {"scrolllock",  VK_SCROLL},
    {"printscreen", VK_SNAPSHOT},
    {"prtsc",       VK_SNAPSHOT},
    {"pause",       VK_PAUSE},
};

bool eq_ci(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

WORD parse_key_name(std::string_view name) {
    if (name.empty()) return 0;

    // F1-F24
    if ((name[0] == 'F' || name[0] == 'f') && name.size() >= 2) {
        unsigned n = 0;
        const auto [p, ec] =
            std::from_chars(name.data() + 1, name.data() + name.size(), n, 10);
        if (ec == std::errc{} && p == name.data() + name.size() &&
            n >= 1 && n <= 24) {
            return static_cast<WORD>(VK_F1 + n - 1);
        }
    }

    // Single ASCII char.
    if (name.size() == 1) {
        char c = name[0];
        if (c >= 'a' && c <= 'z') return static_cast<WORD>(c - 'a' + 'A');
        if (c >= 'A' && c <= 'Z') return static_cast<WORD>(c);
        if (c >= '0' && c <= '9') return static_cast<WORD>(c);
    }

    for (const auto& nk : kNamedKeys) {
        if (eq_ci(name, nk.name)) return nk.vk;
    }
    return 0;
}

// Parse comma-separated modifier list ("ctrl,shift") into VKs.
std::vector<WORD> parse_modifier_list(std::string_view s) {
    std::vector<WORD> out;
    std::size_t start = 0;
    while (start < s.size()) {
        std::size_t end = s.find(',', start);
        if (end == std::string_view::npos) end = s.size();
        WORD vk = parse_key_name(s.substr(start, end - start));
        if (vk) out.push_back(vk);
        start = end + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Argument parsing shared across pointer verbs

bool parse_int(std::string_view s, int& out) {
    const auto* end = s.data() + s.size();
    long v = 0;
    const auto [p, ec] = std::from_chars(s.data(), end, v, 10);
    if (ec != std::errc{} || p != end) return false;
    out = static_cast<int>(v);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// input.move

void move(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 2) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"input.move requires <x> <y>\"}");
        return;
    }
    int x, y;
    if (!parse_int(req.args[0], x) || !parse_int(req.args[1], y)) {
        conn.writer().write_err(ErrorCode::InvalidArgs,
                                "{\"message\":\"x and y must be integers\"}");
        return;
    }

    if (!SetCursorPos(x, y)) {
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", GetLastError());
        conn.writer().write_err(ErrorCode::NotSupported, detail);
        return;
    }
    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// input.click

void click(Connection& conn, const wire::Request& req) {
    if (req.args.size() < 2) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"input.click requires <x> <y> [--button left|right|middle]\"}");
        return;
    }
    int x, y;
    if (!parse_int(req.args[0], x) || !parse_int(req.args[1], y)) {
        conn.writer().write_err(ErrorCode::InvalidArgs,
                                "{\"message\":\"x and y must be integers\"}");
        return;
    }

    DWORD down = MOUSEEVENTF_LEFTDOWN;
    DWORD up   = MOUSEEVENTF_LEFTUP;
    for (std::size_t i = 2; i < req.args.size(); ++i) {
        if (req.args[i] == "--button" && i + 1 < req.args.size()) {
            const auto& b = req.args[++i];
            if (b == "left")        { down = MOUSEEVENTF_LEFTDOWN;   up = MOUSEEVENTF_LEFTUP; }
            else if (b == "right")  { down = MOUSEEVENTF_RIGHTDOWN;  up = MOUSEEVENTF_RIGHTUP; }
            else if (b == "middle") { down = MOUSEEVENTF_MIDDLEDOWN; up = MOUSEEVENTF_MIDDLEUP; }
            else {
                conn.writer().write_err(ErrorCode::InvalidArgs,
                                        "{\"message\":\"unknown --button\"}");
                return;
            }
        }
    }

    if (!crash_check::check_focus_or_fail(conn)) return;
    if (!uipi::check_foreground_or_fail(conn)) return;

    SetCursorPos(x, y);

    INPUT inputs[2] = {};
    inputs[0].type        = INPUT_MOUSE;
    inputs[0].mi.dwFlags  = down;
    inputs[1].type        = INPUT_MOUSE;
    inputs[1].mi.dwFlags  = up;
    SendInput(2, inputs, sizeof(INPUT));

    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// input.scroll

void scroll(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 3) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"input.scroll requires <x> <y> <delta>\"}");
        return;
    }
    int x, y, delta;
    if (!parse_int(req.args[0], x) ||
        !parse_int(req.args[1], y) ||
        !parse_int(req.args[2], delta)) {
        conn.writer().write_err(ErrorCode::InvalidArgs,
                                "{\"message\":\"x, y, delta must be integers\"}");
        return;
    }

    if (!crash_check::check_focus_or_fail(conn)) return;
    if (!uipi::check_foreground_or_fail(conn)) return;

    SetCursorPos(x, y);

    INPUT in{};
    in.type           = INPUT_MOUSE;
    in.mi.dwFlags     = MOUSEEVENTF_WHEEL;
    in.mi.mouseData   = static_cast<DWORD>(delta * WHEEL_DELTA);
    SendInput(1, &in, sizeof(INPUT));

    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// input.key

void key(Connection& conn, const wire::Request& req) {
    if (req.args.empty()) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"input.key requires <vk> [--modifiers <list>]\"}");
        return;
    }

    const WORD main_vk = parse_key_name(req.args[0]);
    if (main_vk == 0) {
        conn.writer().write_err(ErrorCode::InvalidArgs,
                                "{\"message\":\"unknown key name\"}");
        return;
    }

    std::vector<WORD> mods;
    for (std::size_t i = 1; i < req.args.size(); ++i) {
        if (req.args[i] == "--modifiers" && i + 1 < req.args.size()) {
            mods = parse_modifier_list(req.args[++i]);
        }
    }

    if (!crash_check::check_focus_or_fail(conn)) return;
    if (!uipi::check_foreground_or_fail(conn)) return;

    std::vector<INPUT> inputs;
    inputs.reserve(2 + mods.size() * 2);

    auto push_key = [&](WORD vk, bool down) {
        INPUT in{};
        in.type       = INPUT_KEYBOARD;
        in.ki.wVk     = vk;
        in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        inputs.push_back(in);
    };

    for (WORD m : mods) push_key(m, true);
    push_key(main_vk, true);
    push_key(main_vk, false);
    for (auto it = mods.rbegin(); it != mods.rend(); ++it) push_key(*it, false);

    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// input.type

void type(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"input.type requires <length>\"}");
        return;
    }

    unsigned long long length = 0;
    {
        const auto* end = req.args[0].data() + req.args[0].size();
        const auto [p, ec] = std::from_chars(req.args[0].data(), end, length, 10);
        if (ec != std::errc{} || p != end) {
            conn.writer().write_err(
                ErrorCode::InvalidArgs,
                "{\"message\":\"length must be a non-negative integer\"}");
            return;
        }
    }

    auto payload = conn.reader().read_payload(static_cast<std::size_t>(length));

    if (!crash_check::check_focus_or_fail(conn)) return;
    if (!uipi::check_foreground_or_fail(conn)) return;

    if (payload.empty()) {
        conn.writer().write_ok();
        return;
    }

    // UTF-8 → UTF-16
    const int wlen = MultiByteToWideChar(
        CP_UTF8, 0,
        reinterpret_cast<const char*>(payload.data()),
        static_cast<int>(payload.size()),
        nullptr, 0);
    if (wlen <= 0) {
        conn.writer().write_err(ErrorCode::InvalidArgs,
                                "{\"message\":\"payload is not valid UTF-8\"}");
        return;
    }

    std::wstring wtext(static_cast<std::size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0,
                        reinterpret_cast<const char*>(payload.data()),
                        static_cast<int>(payload.size()),
                        wtext.data(), wlen);

    std::vector<INPUT> inputs;
    inputs.reserve(wtext.size() * 2);
    for (wchar_t c : wtext) {
        INPUT in{};
        in.type       = INPUT_KEYBOARD;
        in.ki.wScan   = c;
        in.ki.dwFlags = KEYEVENTF_UNICODE;
        inputs.push_back(in);
        in.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        inputs.push_back(in);
    }
    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));

    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// input.send_message

void send_message(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 4) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"input.send_message requires <hwnd> <msg> <wparam> <lparam>\"}");
        return;
    }
    HWND target = parse_hwnd(req.args[0]);
    if (!target || !IsWindow(target)) {
        std::string detail = "{";
        json::append_kv_string(detail, "handle", req.args[0]);
        detail += '}';
        conn.writer().write_err(ErrorCode::TargetGone, detail);
        return;
    }

    UINT      msg    = static_cast<UINT>(std::strtoul(req.args[1].c_str(), nullptr, 0));
    WPARAM    wparam = static_cast<WPARAM>(std::strtoull(req.args[2].c_str(), nullptr, 0));
    LPARAM    lparam = static_cast<LPARAM>(std::strtoll(req.args[3].c_str(), nullptr, 0));

    if (!uipi::check_window_or_fail(conn, target)) return;

    SendMessageW(target, msg, wparam, lparam);
    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// input.post_message — non-blocking peer of input.send_message. Posts to the
// window's queue and returns immediately; useful when the target window is
// unresponsive to a synchronous SendMessage.

void post_message(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 4) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"input.post_message requires <hwnd> <msg> <wparam> <lparam>\"}");
        return;
    }
    HWND target = parse_hwnd(req.args[0]);
    if (!target || !IsWindow(target)) {
        std::string detail = "{";
        json::append_kv_string(detail, "handle", req.args[0]);
        detail += '}';
        conn.writer().write_err(ErrorCode::TargetGone, detail);
        return;
    }

    UINT      msg    = static_cast<UINT>(std::strtoul(req.args[1].c_str(), nullptr, 0));
    WPARAM    wparam = static_cast<WPARAM>(std::strtoull(req.args[2].c_str(), nullptr, 0));
    LPARAM    lparam = static_cast<LPARAM>(std::strtoll(req.args[3].c_str(), nullptr, 0));

    if (!uipi::check_window_or_fail(conn, target)) return;

    if (!PostMessageW(target, msg, wparam, lparam)) {
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", GetLastError());
        conn.writer().write_err(ErrorCode::NotSupported, detail);
        return;
    }
    conn.writer().write_ok();
}

}  // namespace remote_hands::input_verbs

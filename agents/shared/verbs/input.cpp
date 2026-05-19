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
// Implements the verbs whose contracts are the spec JSON under
// protocol/spec/verbs/{common,windows}/input.*.json (the single source of
// truth). Registered handlers (see agents/shared/capabilities.cpp):
//
//   input.mouse.click   (U)  {x,y,button,double,triple,clicks,
//                              clicks_interval_ms,duration_ms,modifiers}
//                             -> {synthesised,target_handle,actual_position}
//   input.mouse.move    (U)  {x,y,relative}      -> {actual_position}
//   input.mouse.scroll  (U)  {x,y,delta,horizontal} -> {actual_position}
//   input.keyboard.key  (U)  {vk,duration_ms,modifiers}  -> OK 0
//   input.keyboard.type (U)  {text}                      -> OK 0
//   input.send_message  (U)  {handle,msg,wparam,lparam,wparam_string,
//                              lparam_string,timeout_ms} -> {lresult}
//   input.post_message  (U)  {handle,msg,wparam,lparam,wparam_string,
//                              lparam_string}            -> OK 0
//
// PHASE 2.1 — NAMED-ARG MIGRATION. These handlers no longer index `req.args`
// positionally with ad-hoc `--flag` scanning. Each declares its input_schema
// property list (IN SCHEMA ORDER) and reads each value by NAME through the
// shared SchemaArgs resolver (schema_args.hpp), with the schema's property
// ORDER used as the positional fallback when the caller invoked the verb
// positionally (the v2.2 reference client packs positional calls as
// `{"_args":[...]}`). The `window.*` namespace was the pattern-setter; this
// file follows it (and system.cpp / process.cpp / clipboard.cpp /
// registry.cpp / file.cpp) exactly. Validation emits the same
// ErrorCode::InvalidArgs + {"message":...} ergonomics as before.
//
// v2.2 FOLD-IN (non-binary fields/outputs):
//   * input.mouse.click  (#87): response gains `target_handle` (resolved via
//     WindowFromPoint at the clamped position) and `actual_position` {x,y};
//     `synthesised` is the existing accepted/UIPI-dropped signal. New input
//     fields `double` / `triple` / `clicks` / `clicks_interval_ms` /
//     `duration_ms` / `modifiers` are folded in (x-mutually-exclusive
//     enforced).
//   * input.mouse.move   (#71): `relative` (bool) input — x/y are deltas
//     from GetCursorPos when set; response `actual_position` {x,y}.
//   * input.mouse.scroll (#88): `horizontal` (bool) input — MOUSEEVENTF_HWHEEL
//     vs MOUSEEVENTF_WHEEL; response `actual_position` {x,y}.
//   * input.send_message (#89): response returns `lresult` (integer).
//   The binary string-pointer side-channel (`wparam_string` / `lparam_string`
//   on send_message / post_message) is the Phase-2b binary channel: presence
//   + string shape are validated as named args; the actual buffer marshalling
//   is deferred via the spec-declared invalid_args (not_supported is NOT in
//   either verb's x-errors).
//
// ERROR-CODE DISCIPLINE. Every emission is within the DISPATCHED verb's spec
// x-errors. The pre-2.1 handlers funnelled SetCursorPos / PostMessage Win32
// failures into `not_supported`, which is OUTSIDE every input verb's x-errors
// — corrected to the spec-declared `permission_denied` (carrying the raw
// win32 status for diagnosis). The `crash_check::check_focus_or_fail` guard
// (#46) IS applied in every synthetic-input verb (move/click/scroll/key/type)
// after the UIPI guard and before the first injection: it is a no-op
// returning true until a prior `window.focus` on the connection set a tracked
// target, after which it emits `ERR target_gone` for the genuine
// focused-window-destroyed / foreground-stolen failure mode and the verb
// returns without injecting. `target_gone` is absent from the input verbs'
// spec x-errors; that is a known spec incompleteness tracked for the protocol
// repo (the agent legitimately needs to surface this failure) and is NOT a
// reason to weaken or remove the safety guard. The UIPI guard
// (uipi::check_*_or_fail -> ERR uipi_blocked) is RETAINED: uipi_blocked is
// declared in every input verb's x-errors.
//
// ---------------------------------------------------------------------------
// CANONICAL POINTER-INPUT PATTERN
//
// Pointer verbs (click, scroll, anything with x/y) MUST pack the cursor move
// and the button/wheel event into a SINGLE `SendInput` call, with the move
// using `MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK`
// and `mi.dx / mi.dy` normalised to the [0, 65535] virtual-desktop range.
//
// The seductive but wrong shorter form is:
//
//     SetCursorPos(x, y);                       // (1) move cursor logically
//     SendInput(LEFTDOWN); SendInput(LEFTUP);   // (2) click at "current pos"
//
// `LEFTDOWN`/`LEFTUP` events without coordinate fields fire at whatever the
// cursor's position is at *event-processing time*, not at whatever was
// passed to `SetCursorPos`. Any drift between (1) and (2) — from a mouse
// hook, the DWM compositor, focus-change event, "enhanced pointer
// precision" smoothing, P/Invoke marshaling latency — translates directly
// into click-position drift. On Unity IMGUI surfaces the consequence is
// non-obvious: the click registers, but on the wrong layer (scene canvas
// instead of the panel hitbox).
//
// The coupled form below cannot drift because the coordinates are *part of*
// the move event itself, the events are dispatched atomically in one
// syscall, and the down/up events fire at the cursor position established
// by the move event in the same atomic batch.
//
// See `Documents/LLM Feedback/Claude/my-summer-car-test/` for the
// real-world case that surfaced this; see Microsoft Win32 input docs on
// SendInput for the official guidance on absolute coordinates.

#include "../connection.hpp"
#include "../crash_check.hpp"
#include "../errors.hpp"
#include "../json.hpp"
#include "../log.hpp"
#include "../uipi.hpp"
#include "args.hpp"
#include "schema_args.hpp"

#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// MOUSEEVENTF_HWHEEL is Vista+ (winuser.h). The legacy/XP toolchain
// (v141_xp + the v7.1A-era SDK) predates it, so define it numerically here —
// same precedent as file.cpp defining the Win10-only FILE_ATTRIBUTE_* bits
// numerically for toolchains whose SDK predates them. The spec already
// documents that pre-Vista classic/legacy agents emit a vertical wheel for
// `horizontal: true` (no horizontal-wheel support pre-NT-6); on those builds
// the value is simply never injected by a horizontal-wheel-aware target.
#ifndef MOUSEEVENTF_HWHEEL
#define MOUSEEVENTF_HWHEEL 0x01000
#endif

namespace remote_hands::input_verbs {

// The Phase-2.1 named-argument resolver and its invalid_args helper live in
// the shared header (schema_args.hpp) so every namespace reads through one
// definition. Pull them into this TU's unqualified name lookup; behaviour is
// identical to window.cpp / system.cpp / process.cpp / clipboard.cpp /
// registry.cpp / file.cpp.
using wire::SchemaArgs;
using wire::invalid_args;

namespace {

// ---------------------------------------------------------------------------
// HWND <-> wire-string helpers (duplicated from window.cpp; will move to a
// shared header when a third namespace needs it).

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
// Key-name -> virtual-key code mapping

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
    {"pageup",      VK_PRIOR},
    {"pgdn",        VK_NEXT},
    {"pagedown",    VK_NEXT},
    {"up",          VK_UP},
    {"down",        VK_DOWN},
    {"left",        VK_LEFT},
    {"right",       VK_RIGHT},
    {"win",         VK_LWIN},
    {"meta",        VK_LWIN},
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

// ---------------------------------------------------------------------------
// `modifiers` is an input_schema ARRAY of strings whose enum is
// ["ctrl","shift","alt","meta"] (input.mouse.click / input.keyboard.key).
// Read via SchemaArgs::node() and walked explicitly (str() is scalar-only by
// design). Resolves each token to its VK via parse_key_name (which knows
// ctrl/shift/alt/win/meta). Returns false (after writing the spec-declared
// invalid_args) on a non-array `modifiers`, a non-string element, or an
// unrecognised modifier token.
bool resolve_modifiers(Connection& conn, const SchemaArgs& args,
                       std::string_view verb, std::vector<WORD>& mods_out) {
    const mcp::JsonValue* n = args.node("modifiers");
    if (n == nullptr) return true;  // absent — no modifiers
    if (!n->is_array()) {
        invalid_args(conn,
                     std::string(verb) +
                     " 'modifiers' must be an array of strings");
        return false;
    }
    for (const mcp::JsonValue& el : n->as_array()) {
        if (!el.is_string()) {
            invalid_args(conn,
                         std::string(verb) +
                         " 'modifiers' entries must be strings "
                         "(ctrl|shift|alt|meta)");
            return false;
        }
        const WORD vk = parse_key_name(el.as_string());
        if (vk == 0) {
            std::string detail = "{";
            json::append_kv_string(
                detail, "message",
                std::string(verb) +
                " 'modifiers' entry is not a recognised modifier "
                "(ctrl|shift|alt|meta)");
            detail += ',';
            json::append_kv_string(detail, "modifier", el.as_string());
            detail += '}';
            conn.writer().write_err(ErrorCode::InvalidArgs, detail);
            return false;
        }
        mods_out.push_back(vk);
    }
    return true;
}

// Parse a Win32 message-class integer arg (msg / wparam / lparam). The spec
// input_schema types these as `integer` but the description states the bridge
// also accepts hex strings ("0x0010") and decimal — SchemaArgs::str() yields
// the lexeme for either a JSON number or a JSON string, and strtoull base 0
// honours an optional 0x prefix. Rejects junk so a malformed value surfaces
// as the verb's spec-declared invalid_args, not a silent 0.
bool parse_msg_int(const std::string& s, unsigned long long& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 0);
    // strtoull on overflow consumes the whole string (so the end-pointer
    // check passes) and returns ULLONG_MAX with errno == ERANGE — without
    // the errno check an out-of-range value would parse as a silent max.
    if (errno == ERANGE || end != s.c_str() + s.size()) return false;
    out = v;
    return true;
}

// Build an absolute-virtual-desktop INPUT move event for (x, y) screen
// coordinates. The result is intended to be the *first* element in a coupled
// SendInput batch — see the canonical-pattern comment at the top of this
// file. dx/dy are normalised to [0, 65535] across the virtual screen, with
// rounding to the nearest pixel-equivalent to avoid systematic bias to one
// side on odd screen widths.
INPUT make_absolute_move(int x, int y) {
    const int virt_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virt_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virt_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virt_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    INPUT in{};
    in.type        = INPUT_MOUSE;
    in.mi.dwFlags  = MOUSEEVENTF_MOVE
                   | MOUSEEVENTF_ABSOLUTE
                   | MOUSEEVENTF_VIRTUALDESK;
    if (virt_w > 0) {
        in.mi.dx = static_cast<LONG>(
            ((static_cast<long long>(x - virt_x) * 65535LL) +
                 static_cast<long long>(virt_w / 2)) / virt_w);
    }
    if (virt_h > 0) {
        in.mi.dy = static_cast<LONG>(
            ((static_cast<long long>(y - virt_y) * 65535LL) +
                 static_cast<long long>(virt_h / 2)) / virt_h);
    }
    return in;
}

// Clamp a requested (x, y) to the virtual-screen rectangle. The spec
// x-output-schema `actual_position` is "the actual position the click/cursor
// landed at; differs from input when off-screen requests are clamped to the
// virtual-screen edge". Returns the clamped coordinates.
void clamp_to_virtual_screen(int& x, int& y) {
    const int virt_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virt_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virt_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virt_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (virt_w > 0) {
        const int max_x = virt_x + virt_w - 1;
        if (x < virt_x) x = virt_x;
        if (x > max_x)  x = max_x;
    }
    if (virt_h > 0) {
        const int max_y = virt_y + virt_h - 1;
        if (y < virt_y) y = virt_y;
        if (y > max_y)  y = max_y;
    }
}

void append_actual_position(std::string& out, int x, int y) {
    json::append_string(out, "actual_position");
    out += ":{";
    json::append_kv_int(out, "x", x);
    out += ',';
    json::append_kv_int(out, "y", y);
    out += '}';
}

}  // namespace

// ---------------------------------------------------------------------------
// input.mouse.move — input_schema (schema order): ["x","y","relative"].
// x-errors: ["uipi_blocked","permission_denied","invalid_args"].
// x-output-schema: {actual_position:{x,y}} required.

void move(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"x", "y", "relative"});
    if (args.reject_unknown(conn)) return;

    // integer32 (not static_cast<int>(integer())) so an out-of-int-range
    // long long from hostile input surfaces as invalid_args rather than
    // silently wrapping into a garbage coordinate fed to Win32.
    auto xv = args.integer32("x");
    auto yv = args.integer32("y");
    if (!xv || !yv) {
        invalid_args(conn, "input.mouse.move requires integer 'x' and 'y'");
        return;
    }

    bool relative = false;   // schema default false
    if (args.present("relative")) {
        auto r = args.boolean("relative");
        if (!r) {
            invalid_args(conn,
                         "input.mouse.move 'relative' must be a boolean");
            return;
        }
        relative = *r;
    }

    int target_x = *xv;
    int target_y = *yv;
    if (relative) {
        // #71 — x/y are deltas added to the current cursor position.
        POINT cur{};
        GetCursorPos(&cur);
        // long long add then int32-clamp to avoid overflow before clamping
        // to the virtual screen anyway.
        long long sx = static_cast<long long>(cur.x) + *xv;
        long long sy = static_cast<long long>(cur.y) + *yv;
        if (sx < INT_MIN) sx = INT_MIN;
        if (sx > INT_MAX) sx = INT_MAX;
        if (sy < INT_MIN) sy = INT_MIN;
        if (sy > INT_MAX) sy = INT_MAX;
        target_x = static_cast<int>(sx);
        target_y = static_cast<int>(sy);
    }

    // input.mouse.move.json x-errors lists uipi_blocked; the foreground
    // window's IL barrier silently drops moves the same as clicks.
    if (!uipi::check_foreground_or_fail(conn)) return;
    // #46 crash/focus-steal guard — no-op until a prior window.focus tracked
    // a target; on destroy/foreground-steal emits ERR target_gone.
    if (!crash_check::check_focus_or_fail(conn)) return;

    clamp_to_virtual_screen(target_x, target_y);

    INPUT in = make_absolute_move(target_x, target_y);
    if (SendInput(1, &in, sizeof(INPUT)) != 1) {
        // SendInput failure on a UIPI-cleared foreground is an
        // access/permission failure. permission_denied is in
        // input.mouse.move.json x-errors (not_supported is NOT).
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", GetLastError());
        conn.writer().write_err(ErrorCode::PermissionDenied, detail);
        return;
    }

    std::string body = "{";
    append_actual_position(body, target_x, target_y);
    body += '}';
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// input.mouse.click — input_schema (schema order): ["x","y","button",
// "double","triple","clicks","clicks_interval_ms","duration_ms","modifiers"].
// x-mutually-exclusive: ["double","triple","clicks","duration_ms"].
// x-errors: ["uipi_blocked","permission_denied","not_found","invalid_args"].
// x-output-schema: {synthesised, target_handle, actual_position:{x,y}}.

void click(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"x", "y", "button", "double", "triple", "clicks",
                          "clicks_interval_ms", "duration_ms", "modifiers"});
    if (args.reject_unknown(conn)) return;

    auto xv = args.integer32("x");
    auto yv = args.integer32("y");
    if (!xv || !yv) {
        invalid_args(conn, "input.mouse.click requires integer 'x' and 'y'");
        return;
    }

    DWORD down = MOUSEEVENTF_LEFTDOWN;
    DWORD up   = MOUSEEVENTF_LEFTUP;
    if (args.present("button")) {
        auto b = args.str("button");
        if (!b) {
            invalid_args(conn,
                         "input.mouse.click 'button' must be a string");
            return;
        }
        if (*b == "left")        { down = MOUSEEVENTF_LEFTDOWN;   up = MOUSEEVENTF_LEFTUP; }
        else if (*b == "right")  { down = MOUSEEVENTF_RIGHTDOWN;  up = MOUSEEVENTF_RIGHTUP; }
        else if (*b == "middle") { down = MOUSEEVENTF_MIDDLEDOWN; up = MOUSEEVENTF_MIDDLEUP; }
        else {
            std::string detail = "{";
            json::append_kv_string(
                detail, "message",
                "input.mouse.click 'button' must be left|right|middle");
            detail += ',';
            json::append_kv_string(detail, "button", *b);
            detail += '}';
            conn.writer().write_err(ErrorCode::InvalidArgs, detail);
            return;
        }
    }

    bool dbl = false;
    if (args.present("double")) {
        auto d = args.boolean("double");
        if (!d) {
            invalid_args(conn,
                         "input.mouse.click 'double' must be a boolean");
            return;
        }
        dbl = *d;
    }
    bool triple = false;
    if (args.present("triple")) {
        auto t = args.boolean("triple");
        if (!t) {
            invalid_args(conn,
                         "input.mouse.click 'triple' must be a boolean");
            return;
        }
        triple = *t;
    }

    bool have_clicks = false;
    int  clicks      = 0;
    if (args.present("clicks")) {
        auto c = args.integer32("clicks");
        if (!c || *c < 2 || *c > 10) {
            invalid_args(conn,
                         "input.mouse.click 'clicks' must be an integer in "
                         "[2, 10]");
            return;
        }
        have_clicks = true;
        clicks = *c;
    }

    bool have_interval = false;
    int  interval_ms   = 0;
    if (args.present("clicks_interval_ms")) {
        auto iv = args.integer32("clicks_interval_ms");
        if (!iv || *iv < 0 || *iv > 10000) {
            invalid_args(conn,
                         "input.mouse.click 'clicks_interval_ms' must be an "
                         "integer in [0, 10000]");
            return;
        }
        have_interval = true;
        interval_ms = *iv;
    }
    // clicks_interval_ms is only valid alongside clicks
    // (input.mouse.click.json: "Only valid when `clicks` is set; rejected
    // with ERR invalid_args otherwise.").
    if (have_interval && !have_clicks) {
        invalid_args(conn,
                     "input.mouse.click 'clicks_interval_ms' is only valid "
                     "when 'clicks' is set");
        return;
    }

    bool have_duration = false;
    int  duration_ms   = 0;
    if (args.present("duration_ms")) {
        auto d = args.integer32("duration_ms");
        if (!d || *d < 0 || *d > 1000) {
            invalid_args(conn,
                         "input.mouse.click 'duration_ms' must be an integer "
                         "in [0, 1000]");
            return;
        }
        have_duration = true;
        duration_ms = *d;
    }

    // x-mutually-exclusive: ["double","triple","clicks","duration_ms"].
    {
        int set_count = (dbl ? 1 : 0) + (triple ? 1 : 0) +
                        (have_clicks ? 1 : 0) + (have_duration ? 1 : 0);
        if (set_count > 1) {
            invalid_args(conn,
                         "input.mouse.click 'double', 'triple', 'clicks' and "
                         "'duration_ms' are mutually exclusive");
            return;
        }
    }

    std::vector<WORD> mods;
    if (!resolve_modifiers(conn, args, "input.mouse.click", mods)) return;

    // UIPI guard before any state change. uipi_blocked is in
    // input.mouse.click.json x-errors.
    if (!uipi::check_foreground_or_fail(conn)) return;
    // #46 crash/focus-steal guard — no-op until a prior window.focus tracked
    // a target; on destroy/foreground-steal emits ERR target_gone.
    if (!crash_check::check_focus_or_fail(conn)) return;

    int land_x = *xv;
    int land_y = *yv;
    clamp_to_virtual_screen(land_x, land_y);

    auto push_mods_down = [&](std::vector<INPUT>& seq) {
        for (WORD m : mods) {
            INPUT in{};
            in.type   = INPUT_KEYBOARD;
            in.ki.wVk = m;
            seq.push_back(in);
        }
    };
    auto push_mods_up = [&](std::vector<INPUT>& seq) {
        for (auto it = mods.rbegin(); it != mods.rend(); ++it) {
            INPUT in{};
            in.type       = INPUT_KEYBOARD;
            in.ki.wVk     = *it;
            in.ki.dwFlags = KEYEVENTF_KEYUP;
            seq.push_back(in);
        }
    };

    // Determine how many press/release pairs are batched into ONE SendInput
    // call (double = 2, triple = 3, default = 1). `clicks` is a SEPARATE,
    // OS-independent path (N independent SendInput calls separated by
    // clicks_interval_ms) per the spec.
    const int pairs = triple ? 3 : (dbl ? 2 : 1);

    if (have_clicks) {
        // N independent click events; the OS must NOT conflate them. Default
        // interval (when clicks_interval_ms omitted) is explicitly outside
        // the OS double-click window — GetDoubleClickTime() + 50ms.
        const int gap = have_interval
            ? interval_ms
            : static_cast<int>(GetDoubleClickTime()) + 50;
        for (int i = 0; i < clicks; ++i) {
            std::vector<INPUT> seq;
            seq.reserve(3 + mods.size() * 2);
            push_mods_down(seq);
            seq.push_back(make_absolute_move(land_x, land_y));
            INPUT d{}; d.type = INPUT_MOUSE; d.mi.dwFlags = down;
            INPUT u{}; u.type = INPUT_MOUSE; u.mi.dwFlags = up;
            seq.push_back(d);
            seq.push_back(u);
            push_mods_up(seq);
            // #87 synthesised = "accepted by user32's input queue". A
            // short/zero SendInput return means user32 rejected the batch
            // (UIPI/IL barrier or session-input lock) — surface the
            // spec-declared permission_denied rather than reporting an
            // accepted click that never landed. Abort the loop immediately.
            if (SendInput(static_cast<UINT>(seq.size()), seq.data(),
                          sizeof(INPUT)) !=
                    static_cast<UINT>(seq.size())) {
                conn.writer().write_err(
                    ErrorCode::PermissionDenied,
                    "{\"message\":\"SendInput rejected the synthesized "
                    "click\"}");
                return;
            }
            if (i + 1 < clicks && gap > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(gap));
            }
        }
    } else if (have_duration && duration_ms > 0) {
        // Press, hold for duration_ms (request thread blocks), release.
        std::vector<INPUT> down_seq;
        down_seq.reserve(2 + mods.size());
        push_mods_down(down_seq);
        down_seq.push_back(make_absolute_move(land_x, land_y));
        INPUT d{}; d.type = INPUT_MOUSE; d.mi.dwFlags = down;
        down_seq.push_back(d);
        // Check the DOWN batch BEFORE the hold sleep — a rejected press must
        // not sleep then send a phantom UP. #87 synthesised semantics.
        if (SendInput(static_cast<UINT>(down_seq.size()), down_seq.data(),
                      sizeof(INPUT)) !=
                static_cast<UINT>(down_seq.size())) {
            conn.writer().write_err(
                ErrorCode::PermissionDenied,
                "{\"message\":\"SendInput rejected the synthesized "
                "click\"}");
            return;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(duration_ms));

        std::vector<INPUT> up_seq;
        up_seq.reserve(1 + mods.size());
        INPUT u{}; u.type = INPUT_MOUSE; u.mi.dwFlags = up;
        up_seq.push_back(u);
        push_mods_up(up_seq);
        // A failed UP after an accepted DOWN risks a stuck button/modifier —
        // surfacing permission_denied is the correct, observable outcome.
        if (SendInput(static_cast<UINT>(up_seq.size()), up_seq.data(),
                      sizeof(INPUT)) !=
                static_cast<UINT>(up_seq.size())) {
            conn.writer().write_err(
                ErrorCode::PermissionDenied,
                "{\"message\":\"SendInput rejected the synthesized "
                "click\"}");
            return;
        }
    } else {
        // Single SendInput batch: modifiers down, move, then `pairs`
        // press/release pairs (1 / 2 / 3), modifiers up. double/triple batch
        // within GetDoubleClickTime by being in the same atomic call so the
        // OS multi-click handler fires.
        std::vector<INPUT> seq;
        seq.reserve(1 + pairs * 2 + mods.size() * 2);
        push_mods_down(seq);
        seq.push_back(make_absolute_move(land_x, land_y));
        for (int i = 0; i < pairs; ++i) {
            INPUT d{}; d.type = INPUT_MOUSE; d.mi.dwFlags = down;
            INPUT u{}; u.type = INPUT_MOUSE; u.mi.dwFlags = up;
            seq.push_back(d);
            seq.push_back(u);
        }
        push_mods_up(seq);
        // #87 synthesised semantics: a short/zero return means user32
        // rejected the batch — emit the spec-declared permission_denied
        // instead of an OK body claiming an accepted click.
        if (SendInput(static_cast<UINT>(seq.size()), seq.data(),
                      sizeof(INPUT)) !=
                static_cast<UINT>(seq.size())) {
            conn.writer().write_err(
                ErrorCode::PermissionDenied,
                "{\"message\":\"SendInput rejected the synthesized "
                "click\"}");
            return;
        }
    }

    // #87 — response body: synthesised (event accepted by user32's input
    // queue; the UIPI silent-drop post-check already returned ERR
    // uipi_blocked above, so reaching here means accepted -> true),
    // target_handle (WindowFromPoint at the landed position; empty when on
    // the desktop / outside any window), actual_position (the clamped x/y).
    POINT pt{ land_x, land_y };
    HWND under = WindowFromPoint(pt);

    std::string body = "{";
    json::append_kv_bool(body, "synthesised", true);
    body += ',';
    json::append_kv_string(body, "target_handle",
                           under ? hwnd_to_string(under) : std::string());
    body += ',';
    append_actual_position(body, land_x, land_y);
    body += '}';
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// input.mouse.scroll — input_schema (schema order): ["x","y","delta",
// "horizontal"]. x-errors: ["uipi_blocked","permission_denied",
// "invalid_args"]. x-output-schema: {actual_position:{x,y}} required.

void scroll(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"x", "y", "delta", "horizontal"});
    if (args.reject_unknown(conn)) return;

    auto xv = args.integer32("x");
    auto yv = args.integer32("y");
    if (!xv || !yv) {
        invalid_args(conn, "input.mouse.scroll requires integer 'x' and 'y'");
        return;
    }
    auto dv = args.integer32("delta");
    if (!dv) {
        invalid_args(conn,
                     "input.mouse.scroll requires integer 'delta'");
        return;
    }

    bool horizontal = false;   // schema default false
    if (args.present("horizontal")) {
        auto h = args.boolean("horizontal");
        if (!h) {
            invalid_args(conn,
                         "input.mouse.scroll 'horizontal' must be a boolean");
            return;
        }
        horizontal = *h;
    }

    if (!uipi::check_foreground_or_fail(conn)) return;
    // #46 crash/focus-steal guard — no-op until a prior window.focus tracked
    // a target; on destroy/foreground-steal emits ERR target_gone.
    if (!crash_check::check_focus_or_fail(conn)) return;

    int land_x = *xv;
    int land_y = *yv;
    clamp_to_virtual_screen(land_x, land_y);

    // Coupled move + wheel in one SendInput batch — see canonical pattern
    // comment at top of this file and #63. #88: MOUSEEVENTF_HWHEEL for
    // horizontal, MOUSEEVENTF_WHEEL for vertical.
    INPUT inputs[2] = {};
    inputs[0]              = make_absolute_move(land_x, land_y);
    inputs[1].type         = INPUT_MOUSE;
    inputs[1].mi.dwFlags   = horizontal ? MOUSEEVENTF_HWHEEL
                                        : MOUSEEVENTF_WHEEL;
    inputs[1].mi.mouseData =
        static_cast<DWORD>(static_cast<long long>(*dv) * WHEEL_DELTA);
    if (SendInput(2, inputs, sizeof(INPUT)) != 2) {
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", GetLastError());
        conn.writer().write_err(ErrorCode::PermissionDenied, detail);
        return;
    }

    std::string body = "{";
    append_actual_position(body, land_x, land_y);
    body += '}';
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// input.mouse.press — input_schema (schema order): ["button"] (default
// "left"). x-crudx: U. x-errors: ["uipi_blocked","permission_denied",
// "invalid_args"]. x-output-schema: null — wire response is the literal OK 0.
//
// Button down only; no matching up. The button enters this connection's
// held-input set (PROTOCOL.md §2.8) so connection-close cleanup releases it.
// Repeated press of an already-held button is a no-op (the std::set keeps
// membership idempotent; the OS treats a duplicate DOWN the same).

namespace {

// Resolve the optional `button` arg (enum left|right|middle, default left)
// to its MOUSEEVENTF_*DOWN flag. Returns false (after writing the
// spec-declared invalid_args) on a non-string or out-of-enum value.
bool resolve_button_down(Connection& conn, const SchemaArgs& args,
                         std::string_view verb, DWORD& down_out) {
    down_out = MOUSEEVENTF_LEFTDOWN;   // schema default "left"
    if (!args.present("button")) return true;
    auto b = args.str("button");
    if (!b) {
        invalid_args(conn, std::string(verb) + " 'button' must be a string");
        return false;
    }
    if (*b == "left")        down_out = MOUSEEVENTF_LEFTDOWN;
    else if (*b == "right")  down_out = MOUSEEVENTF_RIGHTDOWN;
    else if (*b == "middle") down_out = MOUSEEVENTF_MIDDLEDOWN;
    else {
        std::string detail = "{";
        json::append_kv_string(
            detail, "message",
            std::string(verb) + " 'button' must be left|right|middle");
        detail += ',';
        json::append_kv_string(detail, "button", *b);
        detail += '}';
        conn.writer().write_err(ErrorCode::InvalidArgs, detail);
        return false;
    }
    return true;
}

}  // namespace

void mouse_press(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"button"});
    if (args.reject_unknown(conn)) return;

    DWORD down = MOUSEEVENTF_LEFTDOWN;
    if (!resolve_button_down(conn, args, "input.mouse.press", down)) return;

    // uipi_blocked is in input.mouse.press.json x-errors — the foreground
    // window's IL barrier silently drops the synthesised down the same as a
    // click. Guard before mutating OS / held-input state.
    if (!uipi::check_foreground_or_fail(conn)) return;
    if (!crash_check::check_focus_or_fail(conn)) return;

    INPUT in{};
    in.type       = INPUT_MOUSE;
    in.mi.dwFlags = down;
    if (SendInput(1, &in, sizeof(INPUT)) != 1) {
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", GetLastError());
        conn.writer().write_err(ErrorCode::PermissionDenied, detail);
        return;
    }

    // Register the button as held on this connection (§2.8 cleanup). Stored
    // as the DOWN flag; release/teardown derive the UP flag.
    conn.held_mouse_buttons().insert(static_cast<unsigned long>(down));

    conn.writer().write_ok();  // x-output-schema: null (OK 0)
}

// ---------------------------------------------------------------------------
// input.mouse.release — input_schema (schema order): ["button"] (default
// "left"). x-crudx: U. x-errors: ["permission_denied","invalid_args"]
// (NOT uipi_blocked — the kernel delivers the up regardless; and NOT a
// not_held error — the verb is idempotent cleanup). x-output-schema: null.
//
// Idempotent: always issues the *UP (the OS no-ops gracefully if the button
// was not down) and removes the button from THIS connection's held set as
// well as any other connection's — cross-connection release is the
// documented fail-safe. (Cross-connection visibility into other
// connections' held sets is not plumbed through here; the OS-level *UP is
// the authoritative fail-safe and is always issued, which satisfies the
// "recover from a stuck state" contract.)

void mouse_release(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"button"});
    if (args.reject_unknown(conn)) return;

    DWORD down = MOUSEEVENTF_LEFTDOWN;
    if (!resolve_button_down(conn, args, "input.mouse.release", down)) return;

    // MOUSEEVENTF_*UP is the *DOWN flag << 1 (LEFTDOWN 0x0002 -> LEFTUP
    // 0x0004, etc. — documented Win32 bit layout, same mapping the §2.8
    // teardown uses).
    INPUT in{};
    in.type       = INPUT_MOUSE;
    in.mi.dwFlags = down << 1;
    if (SendInput(1, &in, sizeof(INPUT)) != 1) {
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", GetLastError());
        conn.writer().write_err(ErrorCode::PermissionDenied, detail);
        return;
    }

    conn.held_mouse_buttons().erase(static_cast<unsigned long>(down));

    conn.writer().write_ok();  // x-output-schema: null (OK 0); idempotent
}

// ---------------------------------------------------------------------------
// input.mouse.drag — input_schema (schema order): ["x","y","button","steps"].
// x-crudx: U. required: ["x","y"]. x-errors: ["uipi_blocked",
// "permission_denied","invalid_args"]. x-output-schema: null — OK 0.
//
// Press at the current cursor position, interpolate `steps` MOUSEEVENTF_MOVE
// events to (x, y), release. ONE SendInput batch (down + N moves + up) so
// the OS sees a continuous gesture without real-input interleaving. Atomic:
// the up always fires before the verb returns, so the drag does NOT enter
// the held-input set.

void mouse_drag(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"x", "y", "button", "steps"});
    if (args.reject_unknown(conn)) return;

    auto xv = args.integer32("x");
    auto yv = args.integer32("y");
    if (!xv || !yv) {
        invalid_args(conn, "input.mouse.drag requires integer 'x' and 'y'");
        return;
    }

    DWORD down = MOUSEEVENTF_LEFTDOWN;
    if (!resolve_button_down(conn, args, "input.mouse.drag", down)) return;
    const DWORD up = down << 1;

    int steps = 10;   // schema default
    if (args.present("steps")) {
        auto s = args.integer32("steps");
        if (!s || *s < 1) {
            invalid_args(conn,
                         "input.mouse.drag 'steps' must be an integer >= 1");
            return;
        }
        steps = *s;
    }

    if (!uipi::check_foreground_or_fail(conn)) return;
    if (!crash_check::check_focus_or_fail(conn)) return;

    // Start at the current cursor position; interpolate linearly to the
    // clamped target. The down fires at the start position, then `steps`
    // absolute-virtual-desktop moves walk to (x, y), then the up.
    POINT start{};
    GetCursorPos(&start);

    int end_x = *xv;
    int end_y = *yv;
    clamp_to_virtual_screen(end_x, end_y);

    std::vector<INPUT> seq;
    seq.reserve(static_cast<std::size_t>(steps) + 3);

    // The DOWN event carries no coordinates, so it fires at whatever the live
    // cursor position is when the batch is delivered — which can differ from
    // the `start` snapshotted above if anything moved the pointer between
    // GetCursorPos and SendInput. Prepend an absolute move to the snapshotted
    // `start` so the press lands deterministically at the gesture origin
    // (coordinates carried in the move event, CLAUDE.md #63). The batch is
    // [MOVE(start), DOWN, MOVE(step1)…MOVE(end), UP] — one atomic SendInput.
    seq.push_back(make_absolute_move(start.x, start.y));

    INPUT d{};
    d.type       = INPUT_MOUSE;
    d.mi.dwFlags = down;
    seq.push_back(d);

    for (int i = 1; i <= steps; ++i) {
        // Linear interpolation start -> end; the final step lands exactly on
        // (end_x, end_y). long long intermediate avoids overflow on large
        // coordinate spans before the per-axis division.
        const int px = static_cast<int>(
            start.x + (static_cast<long long>(end_x - start.x) * i) / steps);
        const int py = static_cast<int>(
            start.y + (static_cast<long long>(end_y - start.y) * i) / steps);
        seq.push_back(make_absolute_move(px, py));
    }

    INPUT u{};
    u.type       = INPUT_MOUSE;
    u.mi.dwFlags = up;
    seq.push_back(u);

    if (SendInput(static_cast<UINT>(seq.size()), seq.data(),
                  sizeof(INPUT)) != static_cast<UINT>(seq.size())) {
        // A short/zero return means user32 rejected the batch (UIPI/IL
        // barrier). permission_denied is in input.mouse.drag.json x-errors.
        conn.writer().write_err(
            ErrorCode::PermissionDenied,
            "{\"message\":\"SendInput rejected the synthesized drag\"}");
        return;
    }

    conn.writer().write_ok();  // x-output-schema: null (OK 0)
}

// ---------------------------------------------------------------------------
// input.position — input_schema (schema order): ["include_monitor"] (default
// false). x-crudx: R. x-errors: ["permission_denied"]. x-output-schema:
// {x, y, monitor_index?} — x/y required; monitor_index present iff
// include_monitor:true.

void position(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"include_monitor"});
    if (args.reject_unknown(conn)) return;

    bool include_monitor = false;   // schema default false
    if (args.present("include_monitor")) {
        auto m = args.boolean("include_monitor");
        if (!m) {
            // input.position.json x-errors is ["permission_denied"] only —
            // invalid_args is NOT declared. A malformed include_monitor is
            // a caller fault the agent cannot honour; permission_denied is
            // the closest spec-declared code (carries a reason for
            // diagnosis). The schema default (false) is NOT silently
            // assumed for a present-but-wrong-typed value.
            conn.writer().write_err(
                ErrorCode::PermissionDenied,
                "{\"reason\":\"invalid_include_monitor\",\"message\":"
                "\"input.position 'include_monitor' must be a boolean\"}");
            return;
        }
        include_monitor = *m;
    }

    POINT pt{};
    if (!GetCursorPos(&pt)) {
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", GetLastError());
        conn.writer().write_err(ErrorCode::PermissionDenied, detail);
        return;
    }

    std::string body = "{";
    json::append_kv_int(body, "x", pt.x);
    body += ',';
    json::append_kv_int(body, "y", pt.y);
    if (include_monitor) {
        // Monitor index = position of this monitor's handle in the
        // EnumDisplayMonitors order, which is the same ordering
        // system.info.screens[].index uses. NT4 single-monitor: always 0.
        HMONITOR target =
            MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        struct EnumCtx {
            HMONITOR target;
            int      found;   // -1 until matched
            int      seen;
        } ctx{target, -1, 0};
        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR h, HDC, LPRECT, LPARAM lp) -> BOOL {
                auto* c = reinterpret_cast<EnumCtx*>(lp);
                if (h == c->target) c->found = c->seen;
                ++c->seen;
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));
        const int idx = ctx.found >= 0 ? ctx.found : 0;
        body += ',';
        json::append_kv_int(body, "monitor_index", idx);
    }
    body += '}';
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// input.keyboard.key — input_schema (schema order): ["vk","duration_ms",
// "modifiers"]. x-errors: ["uipi_blocked","permission_denied",
// "invalid_args"]. x-output-schema: null — wire response is the literal OK 0.

void key(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"vk", "duration_ms", "modifiers"});
    if (args.reject_unknown(conn)) return;

    std::optional<std::string> vk_name = args.str("vk");
    if (!vk_name || vk_name->empty()) {
        invalid_args(conn, "input.keyboard.key requires 'vk' (a key name)");
        return;
    }
    const WORD main_vk = parse_key_name(*vk_name);
    if (main_vk == 0) {
        std::string detail = "{";
        json::append_kv_string(detail, "message",
                               "input.keyboard.key 'vk' is not a recognised "
                               "key name");
        detail += ',';
        json::append_kv_string(detail, "vk", *vk_name);
        detail += '}';
        conn.writer().write_err(ErrorCode::InvalidArgs, detail);
        return;
    }

    bool have_duration = false;
    int  duration_ms   = 0;
    if (args.present("duration_ms")) {
        auto d = args.integer32("duration_ms");
        if (!d || *d < 0 || *d > 1000) {
            invalid_args(conn,
                         "input.keyboard.key 'duration_ms' must be an integer "
                         "in [0, 1000]");
            return;
        }
        have_duration = true;
        duration_ms = *d;
    }

    std::vector<WORD> mods;
    if (!resolve_modifiers(conn, args, "input.keyboard.key", mods)) return;

    if (!uipi::check_foreground_or_fail(conn)) return;
    // #46 crash/focus-steal guard — no-op until a prior window.focus tracked
    // a target; on destroy/foreground-steal emits ERR target_gone.
    if (!crash_check::check_focus_or_fail(conn)) return;

    auto push_key = [](std::vector<INPUT>& seq, WORD vk, bool down) {
        INPUT in{};
        in.type       = INPUT_KEYBOARD;
        in.ki.wVk     = vk;
        in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        seq.push_back(in);
    };

    if (have_duration && duration_ms > 0) {
        // Modifiers down + main key down, hold, then main key up + modifiers
        // up. The request thread blocks for the hold (spec: "the connection
        // is wedged for the duration").
        std::vector<INPUT> down_seq;
        down_seq.reserve(1 + mods.size());
        for (WORD m : mods) push_key(down_seq, m, true);
        push_key(down_seq, main_vk, true);
        SendInput(static_cast<UINT>(down_seq.size()), down_seq.data(),
                  sizeof(INPUT));

        std::this_thread::sleep_for(
            std::chrono::milliseconds(duration_ms));

        std::vector<INPUT> up_seq;
        up_seq.reserve(1 + mods.size());
        push_key(up_seq, main_vk, false);
        for (auto it = mods.rbegin(); it != mods.rend(); ++it) {
            push_key(up_seq, *it, false);
        }
        SendInput(static_cast<UINT>(up_seq.size()), up_seq.data(),
                  sizeof(INPUT));
    } else {
        std::vector<INPUT> seq;
        seq.reserve(2 + mods.size() * 2);
        for (WORD m : mods) push_key(seq, m, true);
        push_key(seq, main_vk, true);
        push_key(seq, main_vk, false);
        for (auto it = mods.rbegin(); it != mods.rend(); ++it) {
            push_key(seq, *it, false);
        }
        SendInput(static_cast<UINT>(seq.size()), seq.data(), sizeof(INPUT));
    }

    conn.writer().write_ok();  // x-output-schema: null (OK 0)
}

// ---------------------------------------------------------------------------
// input.keyboard.key_down — input_schema (schema order): ["vk"] (required).
// x-crudx: U. x-errors: ["uipi_blocked","permission_denied","invalid_args"].
// x-output-schema: null — wire response is the literal OK 0.
//
// Press a key and leave it held. The vk enters this connection's held-input
// set (PROTOCOL.md §2.8) so connection-close cleanup releases it. Repeated
// key_down of an already-held key is a no-op (the std::set keeps membership
// idempotent). Same user32-layer-only delivery as input.keyboard.key.

void key_down(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"vk"});
    if (args.reject_unknown(conn)) return;

    std::optional<std::string> vk_name = args.str("vk");
    if (!vk_name || vk_name->empty()) {
        invalid_args(conn,
                     "input.keyboard.key_down requires 'vk' (a key name)");
        return;
    }
    const WORD vk = parse_key_name(*vk_name);
    if (vk == 0) {
        std::string detail = "{";
        json::append_kv_string(detail, "message",
                               "input.keyboard.key_down 'vk' is not a "
                               "recognised key name");
        detail += ',';
        json::append_kv_string(detail, "vk", *vk_name);
        detail += '}';
        conn.writer().write_err(ErrorCode::InvalidArgs, detail);
        return;
    }

    if (!uipi::check_foreground_or_fail(conn)) return;
    if (!crash_check::check_focus_or_fail(conn)) return;

    INPUT in{};
    in.type   = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    // dwFlags 0 == KEYEVENTF_KEYDOWN (no explicit down flag in Win32).
    if (SendInput(1, &in, sizeof(INPUT)) != 1) {
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", GetLastError());
        conn.writer().write_err(ErrorCode::PermissionDenied, detail);
        return;
    }

    conn.held_keys().insert(static_cast<unsigned short>(vk));

    conn.writer().write_ok();  // x-output-schema: null (OK 0)
}

// ---------------------------------------------------------------------------
// input.keyboard.key_up — input_schema (schema order): ["vk"] (required).
// x-crudx: U. x-errors: ["permission_denied","invalid_args"] (NOT
// uipi_blocked — the up event is delivered regardless; and NOT a not_held
// error — the verb is idempotent cleanup). x-output-schema: null — OK 0.
//
// Idempotent: always issues KEYEVENTF_KEYUP (the OS no-ops if the key was
// not down) and removes the vk from this connection's held set. The
// conformance suite asserts key_up on a never-held key returns OkResponse.

void key_up(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"vk"});
    if (args.reject_unknown(conn)) return;

    std::optional<std::string> vk_name = args.str("vk");
    if (!vk_name || vk_name->empty()) {
        invalid_args(conn,
                     "input.keyboard.key_up requires 'vk' (a key name)");
        return;
    }
    const WORD vk = parse_key_name(*vk_name);
    if (vk == 0) {
        std::string detail = "{";
        json::append_kv_string(detail, "message",
                               "input.keyboard.key_up 'vk' is not a "
                               "recognised key name");
        detail += ',';
        json::append_kv_string(detail, "vk", *vk_name);
        detail += '}';
        conn.writer().write_err(ErrorCode::InvalidArgs, detail);
        return;
    }

    INPUT in{};
    in.type       = INPUT_KEYBOARD;
    in.ki.wVk     = vk;
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    if (SendInput(1, &in, sizeof(INPUT)) != 1) {
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", GetLastError());
        conn.writer().write_err(ErrorCode::PermissionDenied, detail);
        return;
    }

    conn.held_keys().erase(static_cast<unsigned short>(vk));

    conn.writer().write_ok();  // x-output-schema: null (OK 0); idempotent
}

// ---------------------------------------------------------------------------
// input.keyboard.type — input_schema: ["text"] (required string; empty is a
// valid no-op). x-errors: ["uipi_blocked","permission_denied",
// "invalid_args"]. x-output-schema: null — wire response is the literal OK 0.
//
// PHASE 2.1: `text` is now a schema string property delivered in the named
// args object (or the positional _args slot), NOT a length-prefixed wire
// payload. The pre-2.1 read_payload(length) path predated the v2.2 schema
// `text` model and is gone.

void type(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"text"});
    if (args.reject_unknown(conn)) return;

    if (!args.present("text")) {
        invalid_args(conn, "input.keyboard.type requires 'text'");
        return;
    }
    std::optional<std::string> text = args.str("text");
    if (!text) {
        invalid_args(conn,
                     "input.keyboard.type 'text' must be a string");
        return;
    }

    if (!uipi::check_foreground_or_fail(conn)) return;
    // #46 crash/focus-steal guard — no-op until a prior window.focus tracked
    // a target; on destroy/foreground-steal emits ERR target_gone.
    if (!crash_check::check_focus_or_fail(conn)) return;

    if (text->empty()) {
        conn.writer().write_ok();   // empty string is a valid no-op
        return;
    }

    // UTF-8 -> UTF-16
    const int wlen = MultiByteToWideChar(
        CP_UTF8, 0, text->data(), static_cast<int>(text->size()),
        nullptr, 0);
    if (wlen <= 0) {
        invalid_args(conn,
                     "input.keyboard.type 'text' is not valid UTF-8");
        return;
    }

    std::wstring wtext(static_cast<std::size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text->data(),
                        static_cast<int>(text->size()),
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
    SendInput(static_cast<UINT>(inputs.size()), inputs.data(),
              sizeof(INPUT));

    conn.writer().write_ok();  // x-output-schema: null (OK 0)
}

// ---------------------------------------------------------------------------
// Shared message-dispatch body for input.send_message and input.post_message.
// Both have input_schema {handle,msg,wparam,lparam,wparam_string,
// lparam_string[,timeout_ms]}; send_message additionally carries timeout_ms
// and returns {lresult}, post_message returns OK 0 (null). They differ in
// x-errors:
//   input.send_message x-errors: not_found, uipi_blocked, permission_denied,
//                                invalid_args, timeout
//   input.post_message x-errors: not_found, uipi_blocked, permission_denied,
//                                invalid_args
// `handle` resolving to no live window is the spec's not_found (in both
// verbs' x-errors). `wparam_string` / `lparam_string` are the Phase-2b
// string-pointer binary side-channel: presence + string shape validated here,
// the buffer marshalling deferred via the spec-declared invalid_args
// (not_supported is in NEITHER verb's x-errors).
namespace {

void message_impl(Connection& conn, const wire::Request& req, bool is_send) {
    const std::string_view verb =
        is_send ? "input.send_message" : "input.post_message";

    // input_schema property lists IN SCHEMA ORDER.
    SchemaArgs args(req, is_send
        ? std::initializer_list<std::string_view>{
              "handle", "msg", "wparam", "lparam",
              "wparam_string", "lparam_string", "timeout_ms"}
        : std::initializer_list<std::string_view>{
              "handle", "msg", "wparam", "lparam",
              "wparam_string", "lparam_string"});
    if (args.reject_unknown(conn)) return;

    std::optional<std::string> handle = args.str("handle");
    if (!handle || handle->empty()) {
        invalid_args(conn,
                     std::string(verb) +
                     " requires 'handle' (win:0x<hex>)");
        return;
    }
    HWND target = parse_hwnd(*handle);
    if (!target || !IsWindow(target)) {
        // handle that resolves to no live window is not_found (in both
        // verbs' x-errors; the pre-2.1 handler emitted target_gone here,
        // which is in NEITHER verb's x-errors — corrected, spec is
        // authoritative, mirrors window.cpp require_target).
        std::string detail = "{";
        json::append_kv_string(detail, "handle", *handle);
        detail += '}';
        conn.writer().write_err(ErrorCode::NotFound, detail);
        return;
    }

    // msg — required. input_schema types it integer; description states the
    // bridge also accepts hex/decimal strings. Reject junk -> invalid_args.
    std::optional<std::string> msg_s = args.str("msg");
    if (!msg_s) {
        invalid_args(conn, std::string(verb) + " requires 'msg'");
        return;
    }
    unsigned long long msg_v = 0;
    if (!parse_msg_int(*msg_s, msg_v)) {
        invalid_args(conn,
                     std::string(verb) +
                     " 'msg' must be a decimal or 0xHEX integer");
        return;
    }
    const UINT msg = static_cast<UINT>(msg_v);

    // wparam_string / lparam_string — Phase-2b string-pointer side-channel.
    // Mutually exclusive with the numeric wparam / lparam respectively.
    const bool has_wp_str = args.present("wparam_string");
    const bool has_lp_str = args.present("lparam_string");
    if (has_wp_str) {
        if (!args.str("wparam_string")) {
            invalid_args(conn,
                         std::string(verb) +
                         " 'wparam_string' must be a string");
            return;
        }
        if (args.present("wparam")) {
            invalid_args(conn,
                         std::string(verb) +
                         " 'wparam' and 'wparam_string' are mutually "
                         "exclusive");
            return;
        }
    }
    if (has_lp_str) {
        if (!args.str("lparam_string")) {
            invalid_args(conn,
                         std::string(verb) +
                         " 'lparam_string' must be a string");
            return;
        }
        if (args.present("lparam")) {
            invalid_args(conn,
                         std::string(verb) +
                         " 'lparam' and 'lparam_string' are mutually "
                         "exclusive");
            return;
        }
    }
    if (has_wp_str || has_lp_str) {
        // PHASE 2b — the string-pointer buffer marshalling (allocate, UTF-16
        // convert, keep alive across the send, free) is the binary
        // side-channel. Arg presence + string shape are validated above; the
        // marshalling is deferred. not_supported is in NEITHER verb's
        // x-errors, so the deferral uses the spec-declared invalid_args with
        // an explicit {"reason":...} (mirrors file.cpp / registry.cpp Phase-2b
        // deferrals).
        std::string detail = "{";
        json::append_kv_string(detail, "reason",
                               "string_pointer_phase2b");
        detail += ',';
        json::append_kv_string(
            detail, "message",
            std::string(verb) +
            " 'wparam_string'/'lparam_string' marshalling is deferred to "
            "Phase 2b; pass numeric 'wparam'/'lparam'");
        detail += '}';
        conn.writer().write_err(ErrorCode::InvalidArgs, detail);
        return;
    }

    // wparam / lparam — optional numeric (default 0). Same hex/decimal-string
    // tolerance as msg.
    WPARAM wparam = 0;
    if (args.present("wparam")) {
        std::optional<std::string> wp = args.str("wparam");
        unsigned long long v = 0;
        if (!wp || !parse_msg_int(*wp, v)) {
            invalid_args(conn,
                         std::string(verb) +
                         " 'wparam' must be a decimal or 0xHEX integer");
            return;
        }
        wparam = static_cast<WPARAM>(v);
    }
    LPARAM lparam = 0;
    if (args.present("lparam")) {
        std::optional<std::string> lp = args.str("lparam");
        if (!lp) {
            invalid_args(conn,
                         std::string(verb) +
                         " 'lparam' must be a decimal or 0xHEX integer");
            return;
        }
        // lparam is signed 64-bit per the spec; accept a leading '-'.
        char* end = nullptr;
        errno = 0;
        const long long sv = std::strtoll(lp->c_str(), &end, 0);
        // strtoll on overflow consumes the whole string and returns
        // LLONG_MIN/MAX with errno == ERANGE; treat as a parse failure (the
        // end-pointer check alone does not catch it).
        if (lp->empty() || errno == ERANGE ||
            end != lp->c_str() + lp->size()) {
            invalid_args(conn,
                         std::string(verb) +
                         " 'lparam' must be a decimal or 0xHEX integer");
            return;
        }
        lparam = static_cast<LPARAM>(sv);
    }

    // UIPI guard — cross-IL sends/posts are silently no-op'd by the OS.
    // uipi_blocked is in both verbs' x-errors.
    if (!uipi::check_window_or_fail(conn, target)) return;

    if (is_send) {
        // timeout_ms — optional, default 5000; 0 disables the timeout
        // (block indefinitely). input.send_message.json x-errors lists
        // timeout.
        DWORD timeout_ms = 5000;
        if (args.present("timeout_ms")) {
            auto t = args.integer("timeout_ms");
            if (!t || *t < 0) {
                invalid_args(conn,
                             "input.send_message 'timeout_ms' must be a "
                             "non-negative integer");
                return;
            }
            // Clamp to DWORD; a very large value behaves as "effectively
            // unbounded" which matches the 0-disables-timeout intent.
            timeout_ms = (*t > 0xFFFFFFFELL)
                ? 0xFFFFFFFEu
                : static_cast<DWORD>(*t);
        }

        DWORD_PTR result = 0;
        // SMTO_ABORTIFHUNG so an unresponsive target surfaces as the
        // spec-declared timeout rather than wedging the request thread
        // indefinitely. timeout_ms == 0 means "block indefinitely" per the
        // spec -> INFINITE.
        const UINT to = (timeout_ms == 0) ? INFINITE : timeout_ms;
        LRESULT sent = SendMessageTimeoutW(
            target, msg, wparam, lparam,
            SMTO_ABORTIFHUNG | SMTO_NORMAL, to, &result);
        if (sent == 0) {
            const DWORD err = GetLastError();
            // SendMessageTimeout returns 0 on timeout with GetLastError 0
            // (or ERROR_TIMEOUT on some builds). Either way, a timed-out
            // synchronous send is the spec's `timeout`.
            if (err == 0 || err == ERROR_TIMEOUT) {
                conn.writer().write_err(
                    ErrorCode::Timeout,
                    "{\"message\":\"target wndproc did not return within "
                    "timeout_ms\"}");
                return;
            }
            char detail[64];
            std::snprintf(detail, sizeof(detail),
                          "{\"win32_error\":%lu}", err);
            conn.writer().write_err(ErrorCode::PermissionDenied, detail);
            return;
        }

        // #89 — x-output-schema: {lresult} required. Signed 64-bit on x64.
        std::string body = "{";
        json::append_kv_int(body, "lresult",
                            static_cast<long long>(
                                static_cast<LONG_PTR>(result)));
        body += '}';
        conn.writer().write_ok(body);
        return;
    }

    // input.post_message — non-blocking; queues and returns immediately.
    if (!PostMessageW(target, msg, wparam, lparam)) {
        const DWORD err = GetLastError();
        if (err == ERROR_INVALID_WINDOW_HANDLE) {
            // The window died between the IsWindow check and the post —
            // not_found is in input.post_message.json x-errors.
            std::string detail = "{";
            json::append_kv_string(detail, "handle", *handle);
            detail += '}';
            conn.writer().write_err(ErrorCode::NotFound, detail);
            return;
        }
        // Any other Win32 failure: closest spec-declared code is
        // permission_denied (in input.post_message.json x-errors;
        // not_supported is NOT). Carries the raw status for diagnosis.
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", err);
        conn.writer().write_err(ErrorCode::PermissionDenied, detail);
        return;
    }
    conn.writer().write_ok();  // x-output-schema: null (OK 0)
}

}  // namespace

// ---------------------------------------------------------------------------
// input.send_message — synchronous SendMessage; returns {lresult}.

void send_message(Connection& conn, const wire::Request& req) {
    message_impl(conn, req, /*is_send=*/true);
}

// ---------------------------------------------------------------------------
// input.post_message — non-blocking PostMessage; returns OK 0.

void post_message(Connection& conn, const wire::Request& req) {
    message_impl(conn, req, /*is_send=*/false);
}

}  // namespace remote_hands::input_verbs

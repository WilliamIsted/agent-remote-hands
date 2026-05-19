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
// Implements the verbs whose contracts are the spec JSON under
// protocol/spec/verbs/common/window.*.json (the single source of truth):
//   window.list   (R)  list/find by input_schema {visible_only,pid,pattern,
//                       include_monitor,limit,full}
//   window.find   (R)  {pattern,match} — substring/prefix/exact/glob/regex
//   window.focus  (U)  {handle} -> {prior_handle,focused_status}
//   window.close  (U)  {handle} -> empty body
//   window.move   (U)  {handle,x,y,w?,h?,foreground?} ->
//                       {bounds,prior_bounds,foreground_status}
//   window.state  (R)  {handle} -> {state}
//
// PHASE 2.1 — NAMED-ARG MIGRATION (pattern-setter for the remaining ~11
// namespaces). These handlers no longer index `req.args` positionally with
// ad-hoc `--flag` scanning. Instead they read each value by its
// input_schema property NAME via the Phase-2.0 accessors in args.hpp, with
// the schema's property ORDER used as the positional fallback when the
// caller invoked the verb positionally (the v2.2 reference client packs
// positional calls as `{"_args":[...]}` — see wire.py _args_to_dict). One
// `SchemaArgs` resolver expresses that "named key, else positional slot"
// rule once; every handler declares its schema property list and reads
// through it. Validation still emits the same ErrorCode::InvalidArgs +
// {"message":...} ergonomics the positional handlers produced.
//
// Window handles are advertised on the wire as `win:0x<hex>` STRINGS.

#include "../connection.hpp"
#include "../errors.hpp"
#include "../json.hpp"
#include "../log.hpp"
#include "../uipi.hpp"
#include "args.hpp"
#include "schema_args.hpp"

#include <cctype>
#include <charconv>
#include <cwctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>   // PathMatchSpecW (glob match mode)

#pragma comment(lib, "shlwapi.lib")

namespace remote_hands::window_verbs {

// The Phase-2.1 named-argument resolver and its invalid_args helper now live
// in the shared header (schema_args.hpp) so the remaining ~11 namespaces read
// through one definition. Pull them into this TU's unqualified name lookup;
// behaviour is identical to the prior in-file definitions.
using wire::SchemaArgs;
using wire::invalid_args;

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

std::wstring get_window_title_w(HWND hwnd) {
    wchar_t buf[512];
    const int len = GetWindowTextW(hwnd, buf, static_cast<int>(std::size(buf)));
    if (len <= 0) return {};
    return std::wstring(buf, static_cast<std::size_t>(len));
}

std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

std::string get_window_title_utf8(HWND hwnd) {
    return wide_to_utf8(get_window_title_w(hwnd));
}

// "Phantom" windows are visible-but-not-meaningful: zero geometry, off-monitor,
// empty title, or marked WS_EX_NOACTIVATE / WS_EX_TOOLWINDOW. Filtered out of
// window.list by default; visible_only:false bypasses.
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

// HMONITOR → 0-based index, ordering matches EnumDisplayMonitors callback
// order (typically primary monitor first on conventional Windows configs).
// Built once per request so window.list / window.find don't pay repeated
// monitor-enum cost per row.
struct MonitorIndexMap {
    std::vector<HMONITOR> ordered;

    int index_of(HMONITOR m) const noexcept {
        for (std::size_t i = 0; i < ordered.size(); ++i) {
            if (ordered[i] == m) return static_cast<int>(i);
        }
        return -1;
    }
};

BOOL CALLBACK collect_monitor(HMONITOR mon, HDC, LPRECT, LPARAM lparam) {
    auto* map = reinterpret_cast<MonitorIndexMap*>(lparam);
    map->ordered.push_back(mon);
    return TRUE;
}

MonitorIndexMap build_monitor_index_map() {
    MonitorIndexMap m;
    EnumDisplayMonitors(nullptr, nullptr, collect_monitor,
                        reinterpret_cast<LPARAM>(&m));
    return m;
}

// Per window.list / window.find x-output-schema: each entry is
//   { handle, title, pid, bounds:{x,y,w,h} [, monitor_index] }
// `bounds` is a declared (optional) property in the list schema and a
// REQUIRED property in the find schema; the post-rc.2 conformance bar
// expects it present on every list entry too, so it is always emitted (a
// schema-valid choice — bounds is a permitted property in both schemas).
// `monitor_index` is added only when include_monitor was requested.
void append_window_object(std::string& out, HWND hwnd,
                          const MonitorIndexMap& monitors,
                          bool include_monitor) {
    RECT r{};
    GetWindowRect(hwnd, &r);

    out += '{';
    json::append_kv_string(out, "handle", hwnd_to_string(hwnd));        out += ',';
    json::append_kv_string(out, "title", get_window_title_utf8(hwnd));  out += ',';
    json::append_kv_uint(out, "pid", get_window_pid(hwnd));             out += ',';
    json::append_string(out, "bounds");
    out += ":{";
    json::append_kv_int(out, "x", r.left);                             out += ',';
    json::append_kv_int(out, "y", r.top);                              out += ',';
    json::append_kv_int(out, "w", r.right - r.left);                   out += ',';
    json::append_kv_int(out, "h", r.bottom - r.top);
    out += '}';
    if (include_monitor) {
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        const int monitor_index = mon ? monitors.index_of(mon) : -1;
        out += ',';
        json::append_kv_int(out, "monitor_index", monitor_index);
    }
    out += '}';
}

void append_bounds_object(std::string& out, const RECT& r) {
    out += '{';
    json::append_kv_int(out, "x", r.left);                             out += ',';
    json::append_kv_int(out, "y", r.top);                              out += ',';
    json::append_kv_int(out, "w", r.right - r.left);                   out += ',';
    json::append_kv_int(out, "h", r.bottom - r.top);
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
//
// Error vocabulary follows each verb's spec `x-errors`: window.focus /
// window.close / window.move / window.state all list `not_found` (+
// `invalid_args`) and NONE list `target_gone`. A missing / non-string
// `handle` is therefore `invalid_args`; a well-formed handle that resolves
// to no live window is `not_found` (NOT `target_gone` — the pre-Phase-2.1
// handler emitted `target_gone` here, which is outside these verbs' declared
// error set; the spec is authoritative so this is corrected).
HWND require_target(Connection& conn, const SchemaArgs& args,
                    std::string_view verb) {
    std::optional<std::string> raw = args.str("handle");
    if (!raw) {
        invalid_args(conn, std::string(verb) + " requires 'handle'");
        return nullptr;
    }
    HWND target = parse_hwnd(*raw);
    if (!target || !IsWindow(target)) {
        std::string detail = "{";
        json::append_kv_string(detail, "handle", *raw);
        detail += '}';
        conn.writer().write_err(ErrorCode::NotFound, detail);
        return nullptr;
    }
    return target;
}

// ---------------------------------------------------------------------------
// window.find match modes (input_schema.match enum)

enum class MatchMode { Substring, Prefix, Exact, Glob, Regex };

std::optional<MatchMode> parse_match_mode(std::string_view m) {
    if (m == "substring") return MatchMode::Substring;
    if (m == "prefix")    return MatchMode::Prefix;
    if (m == "exact")     return MatchMode::Exact;
    if (m == "glob")      return MatchMode::Glob;
    if (m == "regex")     return MatchMode::Regex;
    return std::nullopt;
}

bool icontains(std::wstring_view hay, std::wstring_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    const auto lc = [](wchar_t c) { return std::towlower(c); };
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        std::size_t j = 0;
        for (; j < needle.size(); ++j) {
            if (lc(hay[i + j]) != lc(needle[j])) break;
        }
        if (j == needle.size()) return true;
    }
    return false;
}

bool istartswith(std::wstring_view hay, std::wstring_view prefix) {
    if (prefix.size() > hay.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::towlower(hay[i]) != std::towlower(prefix[i])) return false;
    }
    return true;
}

bool iequals(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    }
    return true;
}

// `regex` is matched against `re`, which find() compiles+validates ONCE
// before EnumWindows (a malformed pattern is rejected as invalid_args up
// front, so the per-window path never recompiles and never sees a *compile*
// error here). For non-regex modes `re` is unused.
//
// A syntactically VALID but pathological pattern (e.g. `(a+)+` against a long
// title) does NOT throw at compile time — MSVC std::regex_search throws
// std::regex_error (error_complexity / error_stack) at SEARCH time. That
// throw must be caught HERE, before it can unwind through the EnumWindows
// C-ABI callback frame: propagating a C++ exception across that frame is
// undefined behaviour on MSVC and crashes the connection thread (a DoS
// triggerable from the unauthenticated wire). So the catch lives inside
// title_matches (called from the callback) — NOT around the EnumWindows()
// call in find(), where the UB would already have happened. The error is
// signalled back via the context flag; the match returns false so
// enumeration unwinds normally. This "catch inside the C-callback, signal
// via context" pattern propagates to every namespace with a regex/predicate
// match mode.
bool title_matches(const std::wstring& title, const std::wstring& pattern,
                    MatchMode mode, const std::wregex& re,
                    bool& regex_error_flag, std::string& regex_error_msg) {
    switch (mode) {
        case MatchMode::Substring: return icontains(title, pattern);
        case MatchMode::Prefix:    return istartswith(title, pattern);
        case MatchMode::Exact:     return iequals(title, pattern);
        case MatchMode::Glob:      return PathMatchSpecW(title.c_str(),
                                                         pattern.c_str()) != 0;
        case MatchMode::Regex:
            try {
                return std::regex_search(title, re);
            } catch (const std::regex_error& e) {
                // Search-time blow-up (catastrophic backtracking / stack
                // exhaustion) on a pattern that compiled cleanly. Catch
                // here so the exception never crosses the EnumWindows
                // C-ABI callback frame (UB on MSVC); signal via the
                // context and report no match so the walk unwinds.
                regex_error_flag = true;
                if (regex_error_msg.empty()) regex_error_msg = e.what();
                return false;
            }
    }
    return false;
}

// ---------------------------------------------------------------------------
// EnumWindows callbacks

struct ListContext {
    bool             visible_only  = true;   // schema default true
    bool             have_pid      = false;
    DWORD            pid_filter    = 0;
    bool             have_pattern  = false;
    std::wstring     pattern;                 // case-insensitive substring
    bool             include_monitor = false;
    int              limit         = 50;      // schema default 50
    std::string      out;
    bool             first         = true;
    MonitorIndexMap  monitors;
    int              count         = 0;
};

BOOL CALLBACK enum_for_list(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<ListContext*>(lparam);

    if (ctx->visible_only && is_phantom_window(hwnd)) return TRUE;

    if (ctx->have_pid && get_window_pid(hwnd) != ctx->pid_filter) return TRUE;

    if (ctx->have_pattern &&
        !icontains(get_window_title_w(hwnd), ctx->pattern)) {
        return TRUE;
    }

    if (ctx->count >= ctx->limit) return FALSE;

    if (!ctx->first) ctx->out += ',';
    ctx->first = false;
    append_window_object(ctx->out, hwnd, ctx->monitors, ctx->include_monitor);
    ++ctx->count;

    return TRUE;
}

struct FindContext {
    std::wstring pattern;
    MatchMode    mode = MatchMode::Substring;
    std::wregex  re;            // compiled+validated once when mode == Regex
    HWND         result = nullptr;
    bool         regex_error_flag = false;   // set by title_matches on a
    std::string  regex_error_msg;            // search-time std::regex_error
};

BOOL CALLBACK enum_for_find(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<FindContext*>(lparam);

    if (is_phantom_window(hwnd)) return TRUE;
    if (!title_matches(get_window_title_w(hwnd), ctx->pattern, ctx->mode,
                       ctx->re, ctx->regex_error_flag,
                       ctx->regex_error_msg)) {
        // Search-time regex blow-up: stop the walk now (no point matching
        // further titles — find() will turn the flag into invalid_args).
        if (ctx->regex_error_flag) return FALSE;
        return TRUE;
    }

    ctx->result = hwnd;
    return FALSE;  // stop enumeration on first match
}

}  // namespace

// ---------------------------------------------------------------------------
// window.list — input_schema {visible_only,pid,pattern,include_monitor,
// limit,full}; x-output-schema is a BARE ARRAY of window objects.

void list(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"visible_only", "pid", "pattern",
                          "include_monitor", "limit", "full"});

    ListContext ctx;
    ctx.monitors = build_monitor_index_map();

    if (args.present("visible_only")) {
        auto v = args.boolean("visible_only");
        if (!v) {
            invalid_args(conn, "window.list 'visible_only' must be a boolean");
            return;
        }
        ctx.visible_only = *v;
    }

    if (args.present("pid")) {
        auto p = args.integer("pid");
        if (!p || *p < 1) {
            invalid_args(conn,
                         "window.list 'pid' must be a positive integer");
            return;
        }
        ctx.have_pid = true;
        ctx.pid_filter = static_cast<DWORD>(*p);
    }

    if (args.present("pattern")) {
        auto pat = args.str("pattern");
        if (!pat) {
            invalid_args(conn, "window.list 'pattern' must be a string");
            return;
        }
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, pat->data(),
                                             static_cast<int>(pat->size()),
                                             nullptr, 0);
        std::wstring w(static_cast<std::size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, pat->data(),
                            static_cast<int>(pat->size()), w.data(), wlen);
        ctx.pattern = std::move(w);
        ctx.have_pattern = true;
    }

    // `include_monitor` and `full` both add monitor_index per the spec
    // (full adds bounds + monitor_index; bounds is always emitted now, so
    // full collapses to "also include monitor_index").
    bool full = false;
    if (args.present("full")) {
        auto f = args.boolean("full");
        if (!f) {
            invalid_args(conn, "window.list 'full' must be a boolean");
            return;
        }
        full = *f;
    }
    if (args.present("include_monitor")) {
        auto im = args.boolean("include_monitor");
        if (!im) {
            invalid_args(conn,
                         "window.list 'include_monitor' must be a boolean");
            return;
        }
        ctx.include_monitor = *im;
    }
    if (full) ctx.include_monitor = true;

    if (args.present("limit")) {
        // integer32 (not static_cast<int>(integer())): an out-of-int-range
        // limit is treated identically to a non-integer — invalid_args, no
        // silent narrowing wrap (ctx.limit is an int gating the enum walk).
        auto l = args.integer32("limit");
        if (!l || *l < 1) {
            invalid_args(conn,
                         "window.list 'limit' must be a positive integer");
            return;
        }
        ctx.limit = *l;
    }

    ctx.out = "[";
    EnumWindows(enum_for_list, reinterpret_cast<LPARAM>(&ctx));
    ctx.out += "]";
    conn.writer().write_ok(ctx.out);
}

// ---------------------------------------------------------------------------
// window.find — input_schema {pattern (required), match}; x-output-schema
// {handle,title,pid,bounds}.

void find(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"pattern", "match"});

    std::optional<std::string> pattern = args.str("pattern");
    if (!pattern || pattern->empty()) {
        invalid_args(conn,
                     "window.find requires 'pattern' (non-empty string)");
        return;
    }

    MatchMode mode = MatchMode::Substring;   // schema default
    if (args.present("match")) {
        auto m = args.str("match");
        if (!m) {
            invalid_args(conn, "window.find 'match' must be a string");
            return;
        }
        auto parsed = parse_match_mode(*m);
        if (!parsed) {
            std::string detail = "{";
            json::append_kv_string(
                detail, "message",
                "window.find 'match' must be one of substring|prefix|"
                "exact|glob|regex");
            detail += ',';
            json::append_kv_string(detail, "match", *m);
            detail += '}';
            conn.writer().write_err(ErrorCode::InvalidArgs, detail);
            return;
        }
        mode = *parsed;
    }

    FindContext ctx;
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, pattern->data(),
                                         static_cast<int>(pattern->size()),
                                         nullptr, 0);
    ctx.pattern.resize(static_cast<std::size_t>(wlen));
    MultiByteToWideChar(CP_UTF8, 0, pattern->data(),
                        static_cast<int>(pattern->size()),
                        ctx.pattern.data(), wlen);
    ctx.mode = mode;

    // Compile + validate the regex ONCE, before the EnumWindows walk. A
    // malformed pattern is a caller error (window.find.json x-errors lists
    // invalid_args), not "no window matched" — surface it as invalid_args
    // rather than letting a per-window recompile swallow it as not_found.
    if (mode == MatchMode::Regex) {
        try {
            ctx.re.assign(ctx.pattern, std::regex::ECMAScript);
        } catch (const std::regex_error& e) {
            invalid_args(conn, "window.find: invalid regex: " +
                                   std::string(e.what()));
            return;
        }
    }

    EnumWindows(enum_for_find, reinterpret_cast<LPARAM>(&ctx));

    // A search-time std::regex_error (catastrophic backtracking / stack
    // exhaustion) on a pattern that compiled cleanly was caught inside the
    // callback (see title_matches) and signalled here. window.find.json
    // x-errors lists invalid_args, so a pathological caller pattern surfaces
    // as invalid_args rather than crashing the connection thread.
    if (ctx.regex_error_flag) {
        invalid_args(conn, "window.find: regex execution error: " +
                               ctx.regex_error_msg);
        return;
    }

    if (ctx.result == nullptr) {
        conn.writer().write_err(ErrorCode::NotFound);
        return;
    }

    const auto monitors = build_monitor_index_map();
    std::string out;
    append_window_object(out, ctx.result, monitors, /*include_monitor=*/false);
    conn.writer().write_ok(out);
}

// ---------------------------------------------------------------------------
// window.focus — input_schema {handle (required)}; x-output-schema
// {prior_handle, focused_status}. lock_held is now success-with-status
// (window.focus.json: "Mirrors window.move's foreground_status shape —
// same condition, success-with-status rather than ERR").

void focus(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"handle"});

    HWND target = require_target(conn, args, "window.focus");
    if (!target) return;

    HWND prior = GetForegroundWindow();

    // Grant ourselves the right to set the foreground window. This won't
    // bypass every lock case (the OS reserves that), but it handles the
    // common case of an idle desktop.
    AllowSetForegroundWindow(ASFW_ANY);

    const bool ok =
        SetForegroundWindow(target) && GetForegroundWindow() == target;

    if (ok) {
        // Record the focused target so subsequent input verbs on this
        // connection can short-circuit with ERR target_gone if the
        // foreground switches away (see crash_check.hpp / closes #46).
        DWORD pid = 0;
        GetWindowThreadProcessId(target, &pid);
        conn.note_focus_target(target, pid);
    }

    std::string body = "{";
    json::append_kv_string(body, "prior_handle",
                           prior ? hwnd_to_string(prior) : std::string());
    body += ',';
    json::append_kv_string(body, "focused_status", ok ? "ok" : "lock_held");
    body += '}';
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// window.close — input_schema {handle (required)}; x-output-schema null
// (empty body — wire response is the literal OK 0).

void close(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"handle"});

    HWND target = require_target(conn, args, "window.close");
    if (!target) return;

    // PostMessage rather than SendMessage so we don't block on an unresponsive
    // target. The window handles WM_CLOSE on its own message pump and may
    // refuse (e.g. unsaved-document prompt).
    PostMessageW(target, WM_CLOSE, 0, 0);
    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// window.move — input_schema {handle,x,y (required), w,h,foreground
// (optional)}; x-output-schema {bounds, prior_bounds, foreground_status}.
// Omitted w/h => SWP_NOSIZE (keep current size, per the spec description).

void move(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"handle", "x", "y", "w", "h", "foreground"});

    HWND target = require_target(conn, args, "window.move");
    if (!target) return;

    // integer32 (not static_cast<int>(integer())) so an out-of-int-range
    // long long from hostile input surfaces here as invalid_args rather than
    // silently wrapping into a garbage coordinate fed to Win32.
    auto xv = args.integer32("x");
    auto yv = args.integer32("y");
    if (!xv || !yv) {
        invalid_args(conn,
                     "window.move requires integer 'x' and 'y'");
        return;
    }

    // w/h optional: omitted => SWP_NOSIZE (preserve current size).
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    int w = 0, h = 0;
    const bool have_w = args.present("w");
    const bool have_h = args.present("h");
    if (have_w != have_h) {
        invalid_args(conn,
                     "window.move 'w' and 'h' must be supplied together");
        return;
    }
    if (have_w) {
        auto wv = args.integer32("w");
        auto hv = args.integer32("h");
        if (!wv || !hv) {
            invalid_args(conn,
                         "window.move 'w' and 'h' must be integers");
            return;
        }
        w = *wv;
        h = *hv;
    } else {
        flags |= SWP_NOSIZE;
    }

    bool want_foreground = false;   // schema default false
    if (args.present("foreground")) {
        auto fg = args.boolean("foreground");
        if (!fg) {
            invalid_args(conn,
                         "window.move 'foreground' must be a boolean");
            return;
        }
        want_foreground = *fg;
    }

    // UIPI guard before any state change: a higher-IL target window rejects
    // the move silently at the kernel boundary. Surface it explicitly as
    // uipi_blocked (window.move.json x-errors), mirroring the input verbs.
    if (!uipi::check_window_or_fail(conn, target)) return;

    RECT prior{};
    GetWindowRect(target, &prior);

    if (!SetWindowPos(target, nullptr, *xv, *yv, w, h, flags)) {
        // Post-UIPI-guard, a SetWindowPos failure on a live window is an
        // access/permission failure. permission_denied is the only remaining
        // declared code in window.move.json x-errors
        // (["not_found","uipi_blocked","permission_denied"]).
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", GetLastError());
        conn.writer().write_err(ErrorCode::PermissionDenied, detail);
        return;
    }

    std::string_view foreground_status = "not_requested";
    if (want_foreground) {
        AllowSetForegroundWindow(ASFW_ANY);
        const bool fg_ok =
            SetForegroundWindow(target) && GetForegroundWindow() == target;
        foreground_status = fg_ok ? "ok" : "lock_held";
        if (fg_ok) {
            DWORD pid = 0;
            GetWindowThreadProcessId(target, &pid);
            conn.note_focus_target(target, pid);
        }
    }

    RECT after{};
    GetWindowRect(target, &after);

    std::string body = "{";
    json::append_string(body, "bounds");
    body += ':';
    append_bounds_object(body, after);
    body += ',';
    json::append_string(body, "prior_bounds");
    body += ':';
    append_bounds_object(body, prior);
    body += ',';
    json::append_kv_string(body, "foreground_status", foreground_status);
    body += '}';
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// window.state — input_schema {handle (required)}; x-output-schema
// {state}. Visibility takes precedence over placement (window.state.json:
// "hidden means the window has WS_VISIBLE clear, not just minimised").

void state(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"handle"});

    HWND target = require_target(conn, args, "window.state");
    if (!target) return;

    std::string_view state_value;
    if (!IsWindowVisible(target)) {
        state_value = "hidden";
    } else {
        WINDOWPLACEMENT wp{};
        wp.length = sizeof(wp);
        if (!GetWindowPlacement(target, &wp)) {
            // window.state.json x-errors = [not_found, invalid_args]. A
            // GetWindowPlacement failure on a require_target-validated handle
            // means the window died between the two calls (TOCTOU) — map to
            // the spec-valid not_found rather than the undeclared
            // not_supported.
            conn.writer().write_err(ErrorCode::NotFound);
            return;
        }
        state_value = show_cmd_to_state(wp.showCmd);
    }

    std::string body = "{";
    json::append_kv_string(body, "state", state_value);
    body += '}';
    conn.writer().write_ok(body);
}

}  // namespace remote_hands::window_verbs

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

#include "crash_check.hpp"

#include "connection.hpp"
#include "errors.hpp"
#include "json.hpp"
#include "protocol.hpp"

#include <cstdio>
#include <cstdint>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace remote_hands::crash_check {

namespace {

std::string format_hwnd(HWND hwnd) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "win:0x%llx",
                  static_cast<unsigned long long>(
                      reinterpret_cast<std::uintptr_t>(hwnd)));
    return buf;
}

void emit_target_gone(Connection& conn, HWND tracked_hwnd,
                      DWORD tracked_pid, const char* reason) {
    std::string detail = "{";
    json::append_kv_string(detail, "handle", format_hwnd(tracked_hwnd));
    detail += ',';
    json::append_kv_uint(detail, "pid", tracked_pid);
    detail += ',';
    json::append_kv_string(detail, "last_known_state", reason);
    detail += '}';
    conn.writer().write_err(ErrorCode::TargetGone, detail);
}

}  // namespace

bool check_focus_or_fail(Connection& conn) {
    const HWND  tracked_hwnd = conn.focus_track_hwnd();
    const DWORD tracked_pid  = conn.focus_track_pid();

    // Nothing tracked yet (no successful window.focus on this connection).
    // Per the design, the contract only kicks in once the caller has told us
    // what they're targeting.
    if (tracked_pid == 0) return true;

    if (!IsWindow(tracked_hwnd)) {
        emit_target_gone(conn, tracked_hwnd, tracked_pid, "window_destroyed");
        return false;
    }

    const HWND fg = GetForegroundWindow();
    DWORD fg_pid  = 0;
    if (fg) GetWindowThreadProcessId(fg, &fg_pid);

    if (fg_pid != tracked_pid) {
        emit_target_gone(conn, tracked_hwnd, tracked_pid, "foreground_changed");
        return false;
    }

    return true;
}

}  // namespace remote_hands::crash_check

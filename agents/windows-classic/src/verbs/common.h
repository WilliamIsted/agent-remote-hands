/*   Copyright 2026 William Isted and contributors
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

/* Shared helpers for the W9 OS verb handlers: response emitters (the
 * connection.c err_kv/ok_kv equivalents are static there), a reusable
 * flag/positional argument parser that rejects unknown --flags with the
 * {"unknown_flag":"..."} detail the conformance suite asserts, win: handle
 * parse/format, and a GetLastError -> wire-code mapper.
 *
 * NOTE: no base64 helper. The W9 prompt sketched base64 for screen.capture,
 * but the real contract (tests/conformance/test_screen.py + test_file.py)
 * returns RAW image / file bytes in the OK payload (`OK <len>\n<bytes>`),
 * not JSON-embedded base64. Raw bytes go out via rh_send_ok_bytes(). */

#ifndef RH_VERBS_COMMON_H
#define RH_VERBS_COMMON_H

#include "../connection.h"
#include "../json.h"

#include <windows.h>

/* --- response emitters (thin wrappers over protocol.c writers) --------- */

void rh_ok(RhConn* c);
void rh_ok_json(RhConn* c, const char* json);
void rh_ok_bytes(RhConn* c, const char* data, int len);
void rh_err(RhConn* c, const char* code);
void rh_err_msg(RhConn* c, const char* code, const char* message);
void rh_err_kv(RhConn* c, const char* code,
               const char* key, const char* value);
void rh_err_unknown_flag(RhConn* c, const char* flag);

/* --- argument / flag parser ------------------------------------------- */

#define RH_MAX_FLAGS 12

/* has_val kinds */
#define RH_FLAG_SWITCH 0  /* bare switch, no value                         */
#define RH_FLAG_VALUE  1  /* consumes the following token as its value     */
#define RH_FLAG_BOOL   2  /* boolean: bare (= true) or followed by an
                           * explicit "true"/"false" token, which is
                           * consumed. Read it with rh_arg_bool().          */

typedef struct {
    const char* name;     /* e.g. "--format"                              */
    int         has_val;  /* RH_FLAG_SWITCH / RH_FLAG_VALUE / RH_FLAG_BOOL */
} RhFlagDef;

typedef struct {
    const char* pos[RH_MAX_ARGS];   /* positional args in order            */
    int         npos;
    const char* val[RH_MAX_FLAGS];  /* parallel to defs[]: value or NULL   */
    int         seen[RH_MAX_FLAGS]; /* parallel to defs[]: flag present?   */
    const char* unknown;            /* first unrecognised --flag, or NULL  */
    int         missing_val;        /* a value-flag had no following token */
} RhArgs;

/* Tokenise req->args against `defs`. A token that matches a known flag
 * name is a flag (value-flags consume the next token). Flag names match
 * with '-' and '_' treated as equal past the leading "--": the v2.1 suite
 * sends "--timeout-ms" while spec-derived (v2.2 / MCP) callers send
 * "--timeout_ms", and both name the same `timeout_ms` property. A token that
 * begins with "--" but matches no def is recorded in out->unknown; anything
 * else is a positional. Negative numbers ("-9999") are positionals (single
 * dash). Always inspect out->unknown first and emit
 * rh_err_unknown_flag() if set -- this is the conformance contract. */
void rh_args_parse(const RhRequest* req, const RhFlagDef* defs,
                   int ndefs, RhArgs* out);

/* Value of boolean flag `idx`: `dflt` when absent, 1 when bare or "true",
 * 0 when "false". */
int  rh_arg_bool(const RhArgs* a, int idx, int dflt);

/* Value of flag `idx` if given, else positional `pos` if present, else
 * NULL. For required spec args that the v2.1 grammar takes positionally
 * but spec-derived callers may send named (e.g. `--path X`). */
const char* rh_arg_named_or_pos(const RhArgs* a, int idx, int pos);

/* Parse a decimal integer token strictly (optional leading '-'). Returns 1
 * and sets *out on success, 0 if `s` is NULL, empty or has trailing junk. */
int  rh_parse_long(const char* s, long* out);

/* Case-insensitive glob match: '*' (any run) and '?' (any one char). */
int  rh_glob_match(const char* pattern, const char* s);

/* Case-insensitive substring test. */
int  rh_contains_ci(const char* haystack, const char* needle);

/* --- win: window-handle helpers --------------------------------------- */

/* Parse "win:0x1A2B" (hex, case-insensitive, "0x" optional). Returns 1 and
 * sets *out on success; 0 if the string is not a well-formed win: handle. */
int  rh_hwnd_parse(const char* s, HWND* out);

/* Format an HWND as "win:0x<HEX>" into `buf` (cap bytes). */
void rh_hwnd_format(HWND h, char* buf, int cap);

/* --- error mapping ----------------------------------------------------- */

/* Map a GetLastError() value to a stable wire error code (PROTOCOL.md 5).
 * Unmapped errors fall back to "permission_denied" (the most common
 * OS-level denial the verbs surface). */
const char* rh_win32_code(DWORD err);

/* Convert a Win32 FILETIME to Unix epoch seconds (used by file.stat and
 * directory.list/stat for the *_unix_s fields). 0 if before 1970. */
unsigned long rh_filetime_unix(const FILETIME* ft);

/* --- filesystem helpers (file.* / directory.*) ------------------------ */

/* Spec `type` for a set of file attributes: link / directory / file. */
const char* rh_attr_type(DWORD attr);

/* Emit the spec `flags` array value for a set of file attributes (call
 * right after rh_json_key(j, "flags")). */
void rh_json_attr_flags(RhJson* j, DWORD attr);

/* Emit the shared stat members -- type, size, mtime_unix_s, ctime_unix_s,
 * atime_unix_s, flags -- into the currently open JSON object. */
void rh_json_find_stat(RhJson* j, const WIN32_FIND_DATAA* fd);

/* dir + "\\" + name into out (cap bytes); tolerates a trailing separator
 * already on `dir`. */
void rh_path_join(const char* dir, const char* name, char* out, int cap);

/* "." or ".." */
int  rh_is_dot(const char* name);

/* --- monitors (system.info.screens / window.list monitor_index) ------- */

#define RH_MAX_SCREENS 16

typedef struct {
    RECT bounds;
    int  primary;   /* first monitor EnumDisplayMonitors reports */
} RhScreen;

/* Fill `out` in EnumDisplayMonitors order (index = position). On NT 4 /
 * Win95, which lack the multi-monitor API, reports the one primary
 * display from GetSystemMetrics. Returns the count (>= 1). */
int rh_screens(RhScreen* out, int max);

/* Index (as rh_screens) of the monitor nearest `pt`; 0 when the
 * multi-monitor API is unavailable. */
int rh_point_monitor_index(POINT pt);

/* Emit a spec Bounds object {x,y,w,h} for `rc` as the current value. */
void rh_json_bounds(RhJson* j, const RECT* rc);

#endif /* RH_VERBS_COMMON_H */

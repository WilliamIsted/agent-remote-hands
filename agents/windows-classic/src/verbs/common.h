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

#define RH_MAX_FLAGS 8

typedef struct {
    const char* name;     /* e.g. "--format"                              */
    int         has_val;  /* 1 => consumes the following token as a value */
} RhFlagDef;

typedef struct {
    const char* pos[RH_MAX_ARGS];   /* positional args in order            */
    int         npos;
    const char* val[RH_MAX_FLAGS];  /* parallel to defs[]: value or NULL   */
    int         seen[RH_MAX_FLAGS]; /* parallel to defs[]: flag present?   */
    const char* unknown;            /* first unrecognised --flag, or NULL  */
    int         missing_val;        /* a value-flag had no following token */
} RhArgs;

/* Tokenise req->args against `defs`. A token that exactly matches a known
 * flag name is a flag (value-flags consume the next token); a token that
 * begins with "--" but matches no def is recorded in out->unknown; anything
 * else is a positional. Negative numbers ("-9999") are positionals (single
 * dash). Always inspect out->unknown first and emit
 * rh_err_unknown_flag() if set -- this is the conformance contract. */
void rh_args_parse(const RhRequest* req, const RhFlagDef* defs,
                   int ndefs, RhArgs* out);

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
 * directory.list/stat for the mtime_unix field). 0 if before 1970. */
unsigned long rh_filetime_unix(const FILETIME* ft);

#endif /* RH_VERBS_COMMON_H */

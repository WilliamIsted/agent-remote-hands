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

// `clipboard.*` namespace verb handlers.
//
// Implements the verbs whose contracts are the spec JSON under
// protocol/spec/verbs/common/clipboard.*.json (the single source of truth):
//   clipboard.get  (R)  input_schema {format (enum ["text"], default
//                       "text")} -> {content, format} (both required).
//                       x-errors: ["empty","permission_denied"].
//   clipboard.set  (U)  input_schema {content (required, string), format
//                       (enum ["text"], default "text")} -> {format}
//                       (required). x-errors: ["permission_denied",
//                       "invalid_args"].
//
// PHASE 2.1 — NAMED-ARG MIGRATION. These handlers no longer index
// `req.args` positionally. Each declares its input_schema property list (IN
// SCHEMA ORDER) and reads each value by NAME through the shared SchemaArgs
// resolver (schema_args.hpp), with the schema's property ORDER used as the
// positional fallback when the caller invoked the verb positionally (the
// v2.2 reference client packs positional calls as `{"_args":[...]}`). The
// `window.*` namespace was the pattern-setter; this file follows it (and
// system.cpp / process.cpp) exactly. Validation emits the same
// ErrorCode::InvalidArgs + {"message":...} ergonomics as before.
//
// v2.2 fold-ins (per the spec JSON, not the old §4.9 wording):
//   * clipboard.set `content` is now an inline UTF-8 STRING property — the
//     pre-Phase-2.1 separate length-prefixed wire payload (read off a
//     `<length>` arg via reader().read_payload()) is gone, mirroring how the
//     process slice replaced `--stdin <length>` with an inline `stdin`
//     string.
//   * Both verbs gain an optional `format` enum (`["text"]`, default
//     "text"). Only `text` is defined today; the response now ECHOES the
//     format. clipboard.get's response is the structured object
//     {content, format} (was a bare text payload); clipboard.set's response
//     is {format} (was an empty body).
//   * Binary / non-text clipboard formats (image, files, base64) are
//     PHASE 2b, NOT this slice. The spec `format` enum is `["text"]` only
//     today (no `content_b64` / binary path is defined), so there is nothing
//     to defer beyond the reserved enum — the text path is implemented in
//     full. See report.
//
// Plain-text only via CF_UNICODETEXT; richer formats (HTML, image, files)
// are not part of v2.2.

#include "../connection.hpp"
#include "../errors.hpp"
#include "../json.hpp"
#include "../log.hpp"
#include "args.hpp"
#include "schema_args.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace remote_hands::clipboard_verbs {

// The Phase-2.1 named-argument resolver and its invalid_args helper live in
// the shared header (schema_args.hpp) so every namespace reads through one
// definition. Pull them into this TU's unqualified name lookup; behaviour is
// identical to window.cpp / system.cpp / process.cpp.
using wire::SchemaArgs;
using wire::invalid_args;

namespace {

class ClipboardLock {
public:
    explicit ClipboardLock(HWND owner = nullptr) {
        // Retry briefly: clipboard lock contention is common but usually
        // releases within a few milliseconds.
        for (int i = 0; i < 5; ++i) {
            if (OpenClipboard(owner)) { ok_ = true; return; }
            Sleep(10);
        }
    }
    ~ClipboardLock() { if (ok_) CloseClipboard(); }
    explicit operator bool() const noexcept { return ok_; }
    ClipboardLock(const ClipboardLock&)            = delete;
    ClipboardLock& operator=(const ClipboardLock&) = delete;
private:
    bool ok_ = false;
};

// The only `format` value defined by the v2.2 input_schema enum (["text"]).
// Reserved for future formats (image, files) which would be wire-additive
// enum extensions — that binary path is Phase 2b, not this slice.
constexpr std::string_view kFormatText = "text";

// Resolve + validate the shared optional `format` property. Returns false
// (and writes ERR invalid_args) on a malformed / out-of-enum value;
// otherwise the format is the schema default "text". Both verbs share the
// identical {format} enum constraint.
bool resolve_format(Connection& conn, SchemaArgs& args, std::string_view verb) {
    if (!args.present("format")) return true;   // schema default "text"
    auto f = args.str("format");
    if (!f) {
        invalid_args(conn, std::string(verb) + " 'format' must be a string");
        return false;
    }
    if (*f != kFormatText) {
        // input_schema: enum ["text"] — only `text` is defined today.
        std::string detail = "{";
        json::append_kv_string(detail, "message",
                               std::string(verb) +
                               " 'format' must be \"text\" (the only "
                               "format defined today)");
        detail += ',';
        json::append_kv_string(detail, "format", *f);
        detail += '}';
        conn.writer().write_err(ErrorCode::InvalidArgs, detail);
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// clipboard.get — input_schema {format (enum ["text"], default "text")};
// x-output-schema {content, format} (both required, additionalProperties
// false). x-errors: ["empty","permission_denied"].
//
// ERR empty when the clipboard has no CF_UNICODETEXT slot; ERR
// permission_denied when another app holds the clipboard open (transient —
// caller can retry), per the windows-modern x-families description.

void get(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"format"});
    if (!resolve_format(conn, args, "clipboard.get")) return;

    ClipboardLock lock;
    if (!lock) {
        // clipboard.get.json x-errors = ["empty","permission_denied"]; the
        // x-families text says "ERR permission_denied when another app holds
        // the clipboard open (transient — caller can retry)". The
        // pre-Phase-2.1 handler emitted lock_held, which is OUTSIDE this
        // verb's declared error set — the spec is authoritative so corrected
        // to permission_denied.
        conn.writer().write_err(ErrorCode::PermissionDenied,
                                "{\"lock_type\":\"clipboard\"}");
        return;
    }

    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) {
        // No CF_UNICODETEXT slot. clipboard.get.json: "Returns ERR empty if
        // the clipboard has no text"; x-errors = ["empty","permission_denied"].
        conn.writer().write_err(ErrorCode::Empty);
        return;
    }

    const auto* w = static_cast<const wchar_t*>(GlobalLock(handle));
    if (!w) {
        // Slot present but the global could not be locked — no readable text.
        conn.writer().write_err(ErrorCode::Empty);
        return;
    }

    const int wlen = static_cast<int>(std::wcslen(w));
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, w, wlen, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(needed > 0 ? needed : 0), '\0');
    if (needed > 0) {
        WideCharToMultiByte(CP_UTF8, 0, w, wlen,
                            utf8.data(), needed, nullptr, nullptr);
    }

    GlobalUnlock(handle);

    // x-output-schema: {content, format} both required,
    // additionalProperties:false. `format` echoes the (only) format read.
    std::string body = "{";
    json::append_kv_string(body, "content", utf8);
    body += ',';
    json::append_kv_string(body, "format", kFormatText);
    body += '}';
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// clipboard.set — input_schema {content (required, string), format (enum
// ["text"], default "text")}; x-output-schema {format} (required,
// additionalProperties false). x-errors: ["permission_denied",
// "invalid_args"].
//
// `content` is an inline UTF-8 STRING property (v2.2) — the pre-Phase-2.1
// separate length-prefixed wire payload (read via reader().read_payload()
// off a `<length>` arg) is gone, mirroring the process slice's inline
// `stdin`.

void set(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"content", "format"});

    // content (required string per input_schema "required": ["content"]).
    // str() is scalar-only; an object/array node returns nullopt and is
    // rejected as invalid_args (the spec type is string).
    std::optional<std::string> content = args.str("content");
    if (!content) {
        invalid_args(conn,
                     "clipboard.set requires 'content' (a UTF-8 string)");
        return;
    }

    if (!resolve_format(conn, args, "clipboard.set")) return;

    const std::string& payload = *content;

    // UTF-8 → UTF-16. `content` is already-decoded JSON text, so it is valid
    // UTF-8 by construction (the MCP JSON parser produced it); MB_ERR_INVALID_
    // CHARS is intentionally NOT set. MultiByteToWideChar returns the wide
    // length (>0), or 0 for an empty string (valid — sets the clipboard to
    // empty text). It never returns negative, so there is no invalid-UTF-8
    // branch to guard here.
    const int wlen = MultiByteToWideChar(
        CP_UTF8, 0,
        payload.data(),
        static_cast<int>(payload.size()),
        nullptr, 0);

    HGLOBAL hglob = GlobalAlloc(GMEM_MOVEABLE,
                                static_cast<SIZE_T>((wlen + 1) * sizeof(wchar_t)));
    if (!hglob) {
        // Allocation failure: the agent could not stage the value on the
        // caller's behalf. clipboard.set.json x-errors =
        // ["permission_denied","invalid_args"]; this is not a caller-input
        // fault, so permission_denied is the closest spec-declared code (the
        // pre-Phase-2.1 not_supported is OUTSIDE this verb's declared error
        // set — corrected; reported).
        conn.writer().write_err(ErrorCode::PermissionDenied,
                                "{\"reason\":\"alloc_failed\"}");
        return;
    }

    auto* wbuf = static_cast<wchar_t*>(GlobalLock(hglob));
    if (!wbuf) {
        // Freshly-allocated handle should always lock; guard the unconditional
        // wbuf[wlen] write anyway (clipboard.get null-checks GlobalLock too).
        GlobalFree(hglob);
        conn.writer().write_err(ErrorCode::PermissionDenied,
                                "{\"reason\":\"lock_failed\"}");
        return;
    }
    if (wlen > 0) {
        MultiByteToWideChar(CP_UTF8, 0,
                            payload.data(),
                            static_cast<int>(payload.size()),
                            wbuf, wlen);
    }
    wbuf[wlen] = L'\0';
    GlobalUnlock(hglob);

    ClipboardLock lock;
    if (!lock) {
        GlobalFree(hglob);
        // x-families: "ERR permission_denied when another app holds the
        // clipboard open". The pre-Phase-2.1 lock_held is OUTSIDE this
        // verb's declared error set — corrected to the spec-declared
        // permission_denied.
        conn.writer().write_err(ErrorCode::PermissionDenied,
                                "{\"lock_type\":\"clipboard\"}");
        return;
    }

    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, hglob)) {
        // Failure: we still own the global handle.
        GlobalFree(hglob);
        // clipboard.set.json x-errors = ["permission_denied","invalid_args"].
        // A post-EmptyClipboard SetClipboardData failure is the agent unable
        // to complete the write on the caller's behalf -> permission_denied
        // (the pre-Phase-2.1 not_supported is OUTSIDE this verb's declared
        // error set — corrected; reported).
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", GetLastError());
        conn.writer().write_err(ErrorCode::PermissionDenied, detail);
        return;
    }
    // SetClipboardData success transfers ownership of hglob to the OS.

    // x-output-schema: {format} required, additionalProperties:false.
    // Symmetric with clipboard.get's format echo.
    std::string body = "{";
    json::append_kv_string(body, "format", kFormatText);
    body += '}';
    conn.writer().write_ok(body);
}

}  // namespace remote_hands::clipboard_verbs

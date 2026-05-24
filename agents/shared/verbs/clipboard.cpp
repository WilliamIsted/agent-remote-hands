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
//   clipboard.get  (R)  read the system clipboard in a requested format.
//   clipboard.set  (U)  replace the system clipboard contents.
//
// FORMAT DISPATCH. The handlers route on a `format` arg whose enum is
// `text|binary|image|files` (the v2.2 spec currently declares only `text` —
// the other three are wire-additive extensions delivered by this slice and
// will join the published enum in a coordinated spec bump). Each branch is
// a self-contained Win32 sequence; the COM/WIC paths used by `image` share
// the same `image_encode::dib_to_png` / `png_to_dib` helpers used by no
// other namespace (`screen.capture` owns the screen-capture encoder path
// separately and is untouched by this slice).
//
//   text   — CF_UNICODETEXT, UTF-8 round-trip (the only format the v0.3.0
//            rebuild ever supported before this slice; preserved verbatim).
//   binary — GetClipboardData/SetClipboardData on a caller-supplied
//            win32_format_id; payload travels as base64.
//   image  — CF_DIBV5 (preferred) or CF_DIB on get → PNG via WIC;
//            PNG on set → CF_DIB via WIC.
//   files  — CF_HDROP / DragQueryFileW iteration on get;
//            DROPFILES + double-null-terminated UTF-16 path list on set.
//
// PHASE 2.1 — NAMED-ARG MIGRATION. These handlers no longer index
// `req.args` positionally. Each declares its input_schema property list (IN
// SCHEMA ORDER) and reads each value by NAME through the shared SchemaArgs
// resolver (schema_args.hpp), with the schema's property ORDER used as the
// positional fallback when the caller invoked the verb positionally. The
// new format-specific properties (`win32_format_id`, `paths`, `mime`) are
// declared alongside the original `content` / `format` so SchemaArgs'
// strict-unknown-key check does not reject them.
//
// Plain-text on CF_UNICODETEXT; binary/image/files via the respective CF_*
// formats. Richer text formats (CF_HTML, CF_RTF) are not in scope.

#include "../base64.hpp"
#include "../connection.hpp"
#include "../errors.hpp"
#include "../image_encode.hpp"
#include "../json.hpp"
#include "../log.hpp"
#include "args.hpp"
#include "schema_args.hpp"

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>     // DROPFILES
#include <shellapi.h>   // DragQueryFileW

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

constexpr std::string_view kFormatText   = "text";
constexpr std::string_view kFormatBinary = "binary";
constexpr std::string_view kFormatImage  = "image";
constexpr std::string_view kFormatFiles  = "files";

// Resolve + validate the shared optional `format` property. Returns false
// (and writes ERR invalid_args) on a malformed / out-of-enum value;
// otherwise writes the resolved string into `format_out` (defaulting to
// "text" when absent, per input_schema default).
bool resolve_format(Connection& conn, SchemaArgs& args,
                    std::string_view verb, std::string& format_out) {
    if (!args.present("format")) {
        format_out.assign(kFormatText);
        return true;
    }
    auto f = args.str("format");
    if (!f) {
        invalid_args(conn, std::string(verb) + " 'format' must be a string");
        return false;
    }
    if (*f != kFormatText && *f != kFormatBinary &&
        *f != kFormatImage && *f != kFormatFiles) {
        std::string detail = "{";
        json::append_kv_string(detail, "message",
                               std::string(verb) +
                               " 'format' must be one of "
                               "\"text\"|\"binary\"|\"image\"|\"files\"");
        detail += ',';
        json::append_kv_string(detail, "format", *f);
        detail += '}';
        conn.writer().write_err(ErrorCode::InvalidArgs, detail);
        return false;
    }
    format_out = *f;
    return true;
}

// UTF-16 -> UTF-8. Returns "" for an empty or unconvertible source (the
// callers either guard length first or are happy with an empty string).
std::string utf16_to_utf8(const wchar_t* w, int wlen) {
    if (wlen <= 0) return {};
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, w, wlen, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, wlen,
                        out.data(), needed, nullptr, nullptr);
    return out;
}

// Write the spec-declared {"lock_type":"clipboard"} permission_denied body.
void write_lock_denied(Connection& conn) {
    conn.writer().write_err(ErrorCode::PermissionDenied,
                            "{\"lock_type\":\"clipboard\"}");
}

// Format-name lookup for the binary path. Standard CF_* values have no
// registered name (GetClipboardFormatNameW returns 0) — emit empty string,
// never null (the JSON contract requires a string field).
std::string clipboard_format_name(UINT cf) {
    wchar_t buf[256];
    const int len = GetClipboardFormatNameW(cf, buf,
                                            static_cast<int>(std::size(buf)));
    if (len <= 0) return {};
    return utf16_to_utf8(buf, len);
}

}  // namespace

// ---------------------------------------------------------------------------
// clipboard.get — dispatches on `format`. x-errors: ["empty","permission_denied"]
// (and "invalid_args" when bad args are passed). The `empty` body carries a
// {"format":"<requested>"} hint so callers can tell "no text on clipboard"
// apart from "no image on clipboard".

void get(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"format", "win32_format_id"});
    if (args.reject_unknown(conn)) return;

    std::string format;
    if (!resolve_format(conn, args, "clipboard.get", format)) return;

    ClipboardLock lock;
    if (!lock) {
        // Spec x-errors include permission_denied for the transient
        // contention case (another app has the clipboard open).
        write_lock_denied(conn);
        return;
    }

    if (format == kFormatText) {
        HANDLE handle = GetClipboardData(CF_UNICODETEXT);
        if (!handle) {
            std::string detail = "{";
            json::append_kv_string(detail, "format", kFormatText);
            detail += '}';
            conn.writer().write_err(ErrorCode::Empty, detail);
            return;
        }
        const auto* w = static_cast<const wchar_t*>(GlobalLock(handle));
        if (!w) {
            std::string detail = "{";
            json::append_kv_string(detail, "format", kFormatText);
            detail += '}';
            conn.writer().write_err(ErrorCode::Empty, detail);
            return;
        }
        const int wlen = static_cast<int>(std::wcslen(w));
        std::string utf8 = utf16_to_utf8(w, wlen);
        GlobalUnlock(handle);

        std::string body = "{";
        json::append_kv_string(body, "content", utf8);
        body += ',';
        json::append_kv_string(body, "format", kFormatText);
        body += '}';
        conn.writer().write_ok(body);
        return;
    }

    if (format == kFormatBinary) {
        // Optional win32_format_id targets a specific format. Absent => pick
        // the first non-text format present on the clipboard.
        UINT cf = 0;
        if (args.present("win32_format_id")) {
            auto v = args.integer("win32_format_id");
            if (!v || *v <= 0 || *v > 0xFFFF) {
                invalid_args(conn,
                             "clipboard.get 'win32_format_id' must be a "
                             "positive 16-bit Win32 clipboard format ID");
                return;
            }
            cf = static_cast<UINT>(*v);
        } else {
            // EnumClipboardFormats(0) returns the first format; iterate
            // until we find one that is not CF_TEXT / CF_UNICODETEXT /
            // CF_OEMTEXT (the text-family formats are handled by the
            // `text` branch, not here).
            UINT current = 0;
            while ((current = EnumClipboardFormats(current)) != 0) {
                if (current == CF_TEXT || current == CF_UNICODETEXT ||
                    current == CF_OEMTEXT) continue;
                cf = current;
                break;
            }
            if (cf == 0) {
                std::string detail = "{";
                json::append_kv_string(detail, "format", kFormatBinary);
                detail += '}';
                conn.writer().write_err(ErrorCode::Empty, detail);
                return;
            }
        }

        HANDLE handle = GetClipboardData(cf);
        if (!handle) {
            std::string detail = "{";
            json::append_kv_string(detail, "format", kFormatBinary);
            detail += ',';
            json::append_kv_uint(detail, "win32_format_id", cf);
            detail += '}';
            conn.writer().write_err(ErrorCode::Empty, detail);
            return;
        }
        const SIZE_T sz = GlobalSize(handle);
        const void* p = GlobalLock(handle);
        if (!p) {
            std::string detail = "{";
            json::append_kv_string(detail, "format", kFormatBinary);
            detail += ',';
            json::append_kv_uint(detail, "win32_format_id", cf);
            detail += '}';
            conn.writer().write_err(ErrorCode::Empty, detail);
            return;
        }
        std::string b64 = base64_encode(
            static_cast<const unsigned char*>(p),
            static_cast<std::size_t>(sz));
        GlobalUnlock(handle);

        const std::string name = clipboard_format_name(cf);

        std::string body = "{";
        json::append_kv_string(body, "content", b64);
        body += ',';
        json::append_kv_string(body, "format", kFormatBinary);
        body += ',';
        json::append_kv_uint(body, "win32_format_id", cf);
        body += ',';
        json::append_kv_string(body, "win32_format_name", name);
        body += '}';
        conn.writer().write_ok(body);
        return;
    }

    if (format == kFormatImage) {
        // CF_DIBV5 first (richer header — preserves colour space); CF_DIB
        // is the universal fallback (every imaging app publishes it).
        HANDLE handle = GetClipboardData(CF_DIBV5);
        if (!handle) handle = GetClipboardData(CF_DIB);
        if (!handle) {
            std::string detail = "{";
            json::append_kv_string(detail, "format", kFormatImage);
            detail += '}';
            conn.writer().write_err(ErrorCode::Empty, detail);
            return;
        }
        const SIZE_T sz = GlobalSize(handle);
        const void* p = GlobalLock(handle);
        if (!p) {
            std::string detail = "{";
            json::append_kv_string(detail, "format", kFormatImage);
            detail += '}';
            conn.writer().write_err(ErrorCode::Empty, detail);
            return;
        }
        std::vector<std::byte> png;
        int width = 0, height = 0;
        const bool ok = image::dib_to_png(
            p, static_cast<std::size_t>(sz), png, width, height);
        GlobalUnlock(handle);

        if (!ok || png.empty()) {
            // The clipboard had a DIB but WIC could not transcode it — the
            // x-errors set has no `decode_failed` code, so map to the
            // spec-declared permission_denied with a machine-readable reason
            // (parallel to the GlobalAlloc-failure pattern in set()).
            conn.writer().write_err(ErrorCode::PermissionDenied,
                                    "{\"reason\":\"dib_decode_failed\"}");
            return;
        }
        std::string b64 = base64_encode(png.data(), png.size());

        std::string body = "{";
        json::append_kv_string(body, "content", b64);
        body += ',';
        json::append_kv_string(body, "format", kFormatImage);
        body += ',';
        json::append_kv_string(body, "mime", "image/png");
        body += ',';
        json::append_kv_int(body, "width", width);
        body += ',';
        json::append_kv_int(body, "height", height);
        body += '}';
        conn.writer().write_ok(body);
        return;
    }

    if (format == kFormatFiles) {
        HANDLE handle = GetClipboardData(CF_HDROP);
        if (!handle) {
            std::string detail = "{";
            json::append_kv_string(detail, "format", kFormatFiles);
            detail += '}';
            conn.writer().write_err(ErrorCode::Empty, detail);
            return;
        }
        const HDROP hdrop = static_cast<HDROP>(handle);
        const UINT count = DragQueryFileW(hdrop, 0xFFFFFFFFu, nullptr, 0);

        std::vector<std::string> paths;
        paths.reserve(count);
        for (UINT i = 0; i < count; ++i) {
            const UINT need = DragQueryFileW(hdrop, i, nullptr, 0);
            std::wstring buf(static_cast<std::size_t>(need) + 1, L'\0');
            const UINT got = DragQueryFileW(hdrop, i, buf.data(),
                                             static_cast<UINT>(buf.size()));
            paths.emplace_back(utf16_to_utf8(buf.data(), static_cast<int>(got)));
        }

        std::string body = "{";
        json::append_string_array(body, "paths", paths);
        body += ',';
        json::append_kv_string(body, "format", kFormatFiles);
        body += '}';
        conn.writer().write_ok(body);
        return;
    }

    // resolve_format() already enforces the enum; this is unreachable.
    invalid_args(conn, "clipboard.get unsupported format");
}

// ---------------------------------------------------------------------------
// clipboard.set — dispatches on `format`. x-errors: ["permission_denied",
// "invalid_args"]. Output shape mirrors get-side: a {format, …} echo body
// so the caller can confirm what landed on the clipboard.

void set(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req,
                    {"content", "format", "win32_format_id", "mime", "paths"});
    if (args.reject_unknown(conn)) return;

    std::string format;
    if (!resolve_format(conn, args, "clipboard.set", format)) return;

    // ---- text -----------------------------------------------------------
    if (format == kFormatText) {
        std::optional<std::string> content = args.str("content");
        if (!content) {
            invalid_args(conn,
                         "clipboard.set requires 'content' (a UTF-8 string)");
            return;
        }
        const std::string& payload = *content;

        const int wlen = MultiByteToWideChar(
            CP_UTF8, 0, payload.data(),
            static_cast<int>(payload.size()), nullptr, 0);

        HGLOBAL hglob = GlobalAlloc(GMEM_MOVEABLE,
                                    static_cast<SIZE_T>((wlen + 1) * sizeof(wchar_t)));
        if (!hglob) {
            conn.writer().write_err(ErrorCode::PermissionDenied,
                                    "{\"reason\":\"alloc_failed\"}");
            return;
        }
        auto* wbuf = static_cast<wchar_t*>(GlobalLock(hglob));
        if (!wbuf) {
            GlobalFree(hglob);
            conn.writer().write_err(ErrorCode::PermissionDenied,
                                    "{\"reason\":\"lock_failed\"}");
            return;
        }
        if (wlen > 0) {
            MultiByteToWideChar(CP_UTF8, 0, payload.data(),
                                static_cast<int>(payload.size()),
                                wbuf, wlen);
        }
        wbuf[wlen] = L'\0';
        GlobalUnlock(hglob);

        ClipboardLock lock;
        if (!lock) {
            GlobalFree(hglob);
            write_lock_denied(conn);
            return;
        }
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, hglob)) {
            GlobalFree(hglob);
            char detail[64];
            std::snprintf(detail, sizeof(detail),
                          "{\"win32_error\":%lu}", GetLastError());
            conn.writer().write_err(ErrorCode::PermissionDenied, detail);
            return;
        }
        // OS owns hglob now.

        std::string body = "{";
        json::append_kv_string(body, "format", kFormatText);
        body += '}';
        conn.writer().write_ok(body);
        return;
    }

    // ---- binary ---------------------------------------------------------
    if (format == kFormatBinary) {
        if (!args.present("win32_format_id")) {
            invalid_args(conn,
                         "clipboard.set 'binary' requires 'win32_format_id'");
            return;
        }
        auto cf_v = args.integer("win32_format_id");
        if (!cf_v || *cf_v <= 0 || *cf_v > 0xFFFF) {
            invalid_args(conn,
                         "clipboard.set 'win32_format_id' must be a positive "
                         "16-bit Win32 clipboard format ID");
            return;
        }
        const UINT cf = static_cast<UINT>(*cf_v);

        std::optional<std::string> content = args.str("content");
        if (!content) {
            invalid_args(conn,
                         "clipboard.set 'binary' requires 'content' "
                         "(base64-encoded bytes)");
            return;
        }
        auto decoded = base64_decode(*content);
        if (!decoded) {
            invalid_args(conn,
                         "clipboard.set 'content' is not valid base64");
            return;
        }
        const std::size_t n = decoded->size();
        HGLOBAL hglob = GlobalAlloc(GMEM_MOVEABLE,
                                    static_cast<SIZE_T>(n == 0 ? 1 : n));
        if (!hglob) {
            conn.writer().write_err(ErrorCode::PermissionDenied,
                                    "{\"reason\":\"alloc_failed\"}");
            return;
        }
        void* p = GlobalLock(hglob);
        if (!p) {
            GlobalFree(hglob);
            conn.writer().write_err(ErrorCode::PermissionDenied,
                                    "{\"reason\":\"lock_failed\"}");
            return;
        }
        if (n > 0) std::memcpy(p, decoded->data(), n);
        GlobalUnlock(hglob);

        ClipboardLock lock;
        if (!lock) {
            GlobalFree(hglob);
            write_lock_denied(conn);
            return;
        }
        EmptyClipboard();
        if (!SetClipboardData(cf, hglob)) {
            GlobalFree(hglob);
            char detail[64];
            std::snprintf(detail, sizeof(detail),
                          "{\"win32_error\":%lu}", GetLastError());
            conn.writer().write_err(ErrorCode::PermissionDenied, detail);
            return;
        }

        std::string body = "{";
        json::append_kv_string(body, "format", kFormatBinary);
        body += ',';
        json::append_kv_uint(body, "win32_format_id", cf);
        body += '}';
        conn.writer().write_ok(body);
        return;
    }

    // ---- image ----------------------------------------------------------
    if (format == kFormatImage) {
        std::optional<std::string> content = args.str("content");
        if (!content) {
            invalid_args(conn,
                         "clipboard.set 'image' requires 'content' "
                         "(base64-encoded PNG bytes)");
            return;
        }
        auto decoded = base64_decode(*content);
        if (!decoded) {
            invalid_args(conn,
                         "clipboard.set 'content' is not valid base64");
            return;
        }
        std::vector<std::byte> dib;
        if (!image::png_to_dib(decoded->data(), decoded->size(), dib) ||
            dib.empty()) {
            invalid_args(conn,
                         "clipboard.set 'image' content could not be decoded "
                         "as a PNG (expected image/png)");
            return;
        }
        HGLOBAL hglob = GlobalAlloc(GMEM_MOVEABLE,
                                    static_cast<SIZE_T>(dib.size()));
        if (!hglob) {
            conn.writer().write_err(ErrorCode::PermissionDenied,
                                    "{\"reason\":\"alloc_failed\"}");
            return;
        }
        void* p = GlobalLock(hglob);
        if (!p) {
            GlobalFree(hglob);
            conn.writer().write_err(ErrorCode::PermissionDenied,
                                    "{\"reason\":\"lock_failed\"}");
            return;
        }
        std::memcpy(p, dib.data(), dib.size());
        GlobalUnlock(hglob);

        ClipboardLock lock;
        if (!lock) {
            GlobalFree(hglob);
            write_lock_denied(conn);
            return;
        }
        EmptyClipboard();
        if (!SetClipboardData(CF_DIB, hglob)) {
            GlobalFree(hglob);
            char detail[64];
            std::snprintf(detail, sizeof(detail),
                          "{\"win32_error\":%lu}", GetLastError());
            conn.writer().write_err(ErrorCode::PermissionDenied, detail);
            return;
        }

        std::string body = "{";
        json::append_kv_string(body, "format", kFormatImage);
        body += '}';
        conn.writer().write_ok(body);
        return;
    }

    // ---- files ----------------------------------------------------------
    if (format == kFormatFiles) {
        // `paths` is a JSON array of strings. The schema-args layer's
        // scalar accessors do not cover arrays — walk the node directly,
        // mirroring input.cpp's `modifiers` resolver.
        const mcp::JsonValue* n = args.node("paths");
        if (n == nullptr) {
            invalid_args(conn,
                         "clipboard.set 'files' requires 'paths' "
                         "(an array of UTF-8 path strings)");
            return;
        }
        if (!n->is_array()) {
            invalid_args(conn,
                         "clipboard.set 'files' 'paths' must be an array of "
                         "strings");
            return;
        }
        std::vector<std::wstring> wpaths;
        for (const mcp::JsonValue& el : n->as_array()) {
            if (!el.is_string()) {
                invalid_args(conn,
                             "clipboard.set 'files' 'paths' entries must be "
                             "strings");
                return;
            }
            const std::string& s = el.as_string();
            const int wlen = MultiByteToWideChar(
                CP_UTF8, 0, s.data(),
                static_cast<int>(s.size()), nullptr, 0);
            std::wstring w(static_cast<std::size_t>(wlen), L'\0');
            if (wlen > 0) {
                MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()),
                                    w.data(), wlen);
            }
            wpaths.push_back(std::move(w));
        }

        // CF_HDROP layout: DROPFILES struct, then a single block of
        // double-null-terminated UTF-16 strings. Each path is followed by a
        // single L'\0'; an empty L"" terminates the block.
        std::size_t chars = 1;  // trailing empty-string terminator
        for (const auto& p : wpaths) chars += p.size() + 1;

        const std::size_t total = sizeof(DROPFILES) + chars * sizeof(wchar_t);
        HGLOBAL hglob = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(total));
        if (!hglob) {
            conn.writer().write_err(ErrorCode::PermissionDenied,
                                    "{\"reason\":\"alloc_failed\"}");
            return;
        }
        auto* base = static_cast<BYTE*>(GlobalLock(hglob));
        if (!base) {
            GlobalFree(hglob);
            conn.writer().write_err(ErrorCode::PermissionDenied,
                                    "{\"reason\":\"lock_failed\"}");
            return;
        }
        auto* df = reinterpret_cast<DROPFILES*>(base);
        std::memset(df, 0, sizeof(DROPFILES));
        df->pFiles = sizeof(DROPFILES);
        df->fWide  = TRUE;                 // UTF-16 path block

        auto* w = reinterpret_cast<wchar_t*>(base + sizeof(DROPFILES));
        for (const auto& p : wpaths) {
            if (!p.empty()) {
                std::memcpy(w, p.data(), p.size() * sizeof(wchar_t));
                w += p.size();
            }
            *w++ = L'\0';
        }
        *w = L'\0';
        GlobalUnlock(hglob);

        ClipboardLock lock;
        if (!lock) {
            GlobalFree(hglob);
            write_lock_denied(conn);
            return;
        }
        EmptyClipboard();
        if (!SetClipboardData(CF_HDROP, hglob)) {
            GlobalFree(hglob);
            char detail[64];
            std::snprintf(detail, sizeof(detail),
                          "{\"win32_error\":%lu}", GetLastError());
            conn.writer().write_err(ErrorCode::PermissionDenied, detail);
            return;
        }

        std::string body = "{";
        json::append_kv_string(body, "format", kFormatFiles);
        body += ',';
        json::append_kv_uint(body, "count",
                             static_cast<unsigned long long>(wpaths.size()));
        body += '}';
        conn.writer().write_ok(body);
        return;
    }

    invalid_args(conn, "clipboard.set unsupported format");
}

}  // namespace remote_hands::clipboard_verbs

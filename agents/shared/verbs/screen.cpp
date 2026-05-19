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
// Implements the verb whose contract is the spec JSON
// protocol/spec/verbs/common/screen.capture.json (the single source of
// truth). Registered handler (see agents/shared/capabilities.cpp):
//
//   screen.capture  (R)  {region,window,monitor,format,quality,cursor,encoding}
//
// PHASE 2.1 — NAMED-ARG MIGRATION. This handler no longer indexes `req.args`
// positionally with ad-hoc `--flag` scanning or comma-token `--region`
// reconstruction. It declares its input_schema property list (IN SCHEMA
// ORDER) and reads each value by NAME through the shared SchemaArgs resolver
// (schema_args.hpp), with the schema's property ORDER used as the positional
// fallback when the caller invoked the verb positionally (the v2.2 reference
// client packs positional calls as `{"_args":[...]}`). The `window.*`
// namespace was the pattern-setter; this file follows it (and system.cpp /
// process.cpp / clipboard.cpp / registry.cpp / file.cpp / input.cpp /
// element.cpp) exactly. `region` is a nested object {x,y,w,h} read via
// args.node() and walked explicitly (str() is scalar-only by design).
//
// v2.2 INPUT fields folded into arg parsing:
//   #66 cursor   (bool)  — composite OS cursor; FULLY APPLIED (drives the
//                           existing include_cursor capture parameter).
//   #83 format   (enum)  — png|webp|bmp|jpeg|heic; parsed + validated.
//       quality  (int)   — 1..100; parsed + validated, applies to lossy
//                           formats only (no spec-mandated default; passed
//                           through where the encoder consumes it).
//   #91 encoding (enum)  — base64|binary; parsed + validated. The default
//                           `base64` behaviour is preserved byte-for-byte.
//                           `binary` is the Protocol v3 PR1.d "Shape B"
//                           side-channel: modern family emits the raw image
//                           bytes out-of-band (JSON result carries
//                           width/height/format + a blob_size the framing
//                           layer appends); non-modern families keep the
//                           unsupported_format response (side-channel not
//                           available on that family).
//   region/window/monitor — selectors with x-mutually-exclusive; region is a
//                           nested {x,y,w,h} object. FULLY APPLIED (drive the
//                           existing capture_* dispatch).
//
// OUTPUT BOUNDARY. The image encoder set is PNG (via WIC) / BMP. Selecting
// `format: webp|jpeg|heic` needs an encoder this build does not ship; that
// is parsed + validated here then surfaced as unsupported_format (see
// ERROR-CODE DISCIPLINE) rather than implemented. The default base64 path
// (png/bmp, framed by the writer) keeps its existing capture + encode
// behaviour byte-for-byte; the modern binary side-channel reuses the same
// captured + encoded bytes, only the emission framing differs.
//
// ERROR-CODE DISCIPLINE. screen.capture.json x-errors is exactly
// ["not_found","unsupported_format","permission_denied"].
//   - not_found            -> ErrorCode::NotFound  (monitor index absent).
//   - unsupported_format   -> ErrorCode::UnsupportedFormat. Emitted for an
//                             unknown format value, a not-bundled
//                             webp/jpeg/heic encoder, and (non-modern
//                             families only) an encoding:binary request
//                             (the side-channel is modern-only; this is the
//                             only in-x-errors code that fits a "this build
//                             can't produce that output form" condition).
//   - permission_denied    -> ErrorCode::PermissionDenied exists; not naturally
//                             reached by the current GDI capture path (no ACL
//                             gate), so it is not emitted here.
//
// Two emissions remain OUTSIDE this verb's x-errors and are tracked (NOT
// slice-introduced — pre-existing capture-pipeline runtime failures, not
// the named-arg refactor's concern; conformance does not exercise them):
//   - capture produced an empty frame  -> NotSupported
//   - the bundled encoder produced no bytes -> NotSupported
// Also tracked for the protocol repo: screen.capture x-errors lacks
// invalid_args though the verb validates region/window/monitor/quality/
// cursor/encoding inputs (same class as protocol#99); arg-shape failures
// emit InvalidArgs (agent correct; validation NOT weakened).
//
// Capture path: BitBlt today (universal). WGC is a future runtime-detected
// fast path documented in COMPATIBILITY.md. Encoder: PNG (default, via WIC)
// and BMP. WebP/JPEG/HEIC are deferred until their encoders are bundled —
// `system.info.capabilities.image_formats` advertises only the formats
// actually available.

#include "../connection.hpp"
#include "../errors.hpp"
#include "../image_encode.hpp"
#include "../json.hpp"
#include "../log.hpp"
#include "../screen_capture.hpp"
#include "args.hpp"
#include "schema_args.hpp"

#include <charconv>
#include <climits>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace remote_hands::screen_verbs {

// The Phase-2.1 named-argument resolver and its invalid_args helper live in
// the shared header (schema_args.hpp) so every namespace reads through one
// definition. Pull them into this TU's unqualified name lookup; behaviour is
// identical to window.cpp / system.cpp / process.cpp / clipboard.cpp /
// registry.cpp / file.cpp / input.cpp / element.cpp.
using wire::SchemaArgs;
using wire::invalid_args;

namespace {

// screen.capture.json `format` enum: ["png","webp","bmp","jpeg","heic"].
// png/bmp have encoders in this build; webp/jpeg/heic are valid enum members
// whose encoders are NOT bundled here (forward-compat / Phase-2b).
enum class Format { Png, Bmp, Webp, Jpeg, Heic };

bool parse_format(std::string_view s, Format& out) {
    if (s == "png")  { out = Format::Png;  return true; }
    if (s == "bmp")  { out = Format::Bmp;  return true; }
    if (s == "webp") { out = Format::Webp; return true; }
    if (s == "jpeg") { out = Format::Jpeg; return true; }
    if (s == "heic") { out = Format::Heic; return true; }
    return false;
}

bool format_has_encoder(Format f) {
    return f == Format::Png || f == Format::Bmp;
}

// screen.capture.json `window` is a string handle, e.g. "win:0x1A2B".
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

// EnumDisplayMonitors callback that selects the Nth monitor (0-based),
// matching the same enumeration order as window.list's monitor_index. A
// plain struct + integer index — no allocation / no throwing operation in
// the callback body, so no C++ exception can cross the Win32 C-ABI boundary.
struct MonitorByIndex {
    int  target_index  = 0;
    int  current_index = 0;
    RECT rect{};
    bool found = false;
};

BOOL CALLBACK find_monitor_by_index(HMONITOR mon, HDC, LPRECT, LPARAM lparam) {
    auto* state = reinterpret_cast<MonitorByIndex*>(lparam);
    if (state->current_index == state->target_index) {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(mon, &info)) {
            state->rect  = info.rcMonitor;
            state->found = true;
        }
        return FALSE;
    }
    state->current_index++;
    return TRUE;
}

}  // namespace

// ---------------------------------------------------------------------------
// screen.capture — input_schema (schema order):
//   ["region","window","monitor","format","quality","cursor","encoding"]
// x-mutually-exclusive: ["region","window","monitor"].
// x-errors: ["not_found","unsupported_format","permission_denied"].
// x-output-schema: image bytes (base64 by default; raw when encoding:binary).

void capture(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"region", "window", "monitor", "format", "quality",
                          "cursor", "encoding"});
    if (args.reject_unknown(conn)) return;

    // --- region: nested object {x,y,w,h}, all required, all integers -------
    bool has_region = false;
    int  rx = 0, ry = 0, rw = 0, rh = 0;
    if (args.present("region")) {
        const mcp::JsonValue* node = args.node("region");
        if (node == nullptr || !node->is_object()) {
            invalid_args(conn,
                         "screen.capture 'region' must be an object "
                         "{x,y,w,h}");
            return;
        }
        // Read each member by name. The members may arrive as JSON numbers
        // (typed client) or numeric strings (reference client); reuse the
        // same lexeme-tolerant integer parse the rest of the slice uses by
        // reading through a per-member SchemaArgs-style lambda.
        struct Member { const char* key; int* out; bool min_one; };
        const Member members[] = {
            {"x", &rx, false}, {"y", &ry, false},
            {"w", &rw, true},  {"h", &rh, true},
        };
        for (const Member& m : members) {
            const mcp::JsonValue* v = node->find(m.key);
            if (v == nullptr || v->is_null()) {
                invalid_args(conn,
                             "screen.capture 'region' requires x,y,w,h");
                return;
            }
            std::optional<std::string> lex;
            if (v->is_number())      lex = v->num_lexeme();
            else if (v->is_string()) lex = v->as_string();
            if (!lex) {
                invalid_args(conn,
                             "screen.capture 'region' x,y,w,h must be "
                             "integers");
                return;
            }
            long long parsed = 0;
            const char* begin = lex->data();
            const char* end   = begin + lex->size();
            if (begin != end && *begin == '+') {
                invalid_args(conn,
                             "screen.capture 'region' x,y,w,h must be "
                             "integers");
                return;
            }
            const auto [p, ec] = std::from_chars(begin, end, parsed, 10);
            if (ec != std::errc{} || p != end ||
                parsed < INT_MIN || parsed > INT_MAX) {
                invalid_args(conn,
                             "screen.capture 'region' x,y,w,h must be "
                             "integers");
                return;
            }
            if (m.min_one && parsed < 1) {
                invalid_args(conn,
                             "screen.capture 'region' w and h must be >= 1");
                return;
            }
            *m.out = static_cast<int>(parsed);
        }
        has_region = true;
    }

    // --- window: string handle "win:0x<hex>" ------------------------------
    bool has_window = false;
    HWND hwnd = nullptr;
    if (args.present("window")) {
        std::optional<std::string> w = args.str("window");
        if (!w) {
            invalid_args(conn,
                         "screen.capture 'window' must be a string "
                         "(win:0x<hex>)");
            return;
        }
        hwnd = parse_hwnd(*w);
        if (!hwnd) {
            invalid_args(conn,
                         "screen.capture 'window' must be win:0x<hex>");
            return;
        }
        has_window = true;
    }

    // --- monitor: zero-based integer index --------------------------------
    bool has_monitor = false;
    int  monitor_index = 0;
    if (args.present("monitor")) {
        auto m = args.integer32("monitor");
        if (!m || *m < 0) {
            invalid_args(conn,
                         "screen.capture 'monitor' must be a non-negative "
                         "integer");
            return;
        }
        monitor_index = *m;
        has_monitor = true;
    }

    // x-mutually-exclusive: region / window / monitor.
    const int selectors =
        (has_region ? 1 : 0) + (has_window ? 1 : 0) + (has_monitor ? 1 : 0);
    if (selectors > 1) {
        invalid_args(conn,
                     "screen.capture 'region' / 'window' / 'monitor' are "
                     "mutually exclusive");
        return;
    }

    // --- format: enum, default "png" --------------------------------------
    Format format = Format::Png;
    if (args.present("format")) {
        auto f = args.str("format");
        if (!f) {
            invalid_args(conn,
                         "screen.capture 'format' must be a string");
            return;
        }
        if (!parse_format(*f, format)) {
            // Not a member of the spec enum (e.g. "tiff-2000", "webp:70").
            // screen.capture.json x-errors declares unsupported_format for
            // exactly this.
            conn.writer().write_err(
                ErrorCode::UnsupportedFormat,
                "{\"reason\":\"unsupported format; advertised in "
                "system.info.capabilities.image_formats\"}");
            return;
        }
    }

    // --- quality: integer 1..100, optional, lossy-only --------------------
    std::optional<int> quality;
    if (args.present("quality")) {
        auto q = args.integer32("quality");
        if (!q || *q < 1 || *q > 100) {
            invalid_args(conn,
                         "screen.capture 'quality' must be an integer in "
                         "[1, 100]");
            return;
        }
        quality = *q;
        // `quality` is silently ignored for png/bmp per the spec; it is
        // validated above regardless so a bad value never reaches a lossy
        // encoder. The current build ships no lossy encoder, so a supplied
        // quality is parsed/validated then unused on the legacy path.
        (void)quality;
    }

    // --- cursor: boolean, default true (#66) ------------------------------
    bool include_cursor = true;  // spec default
    if (args.present("cursor")) {
        auto c = args.boolean("cursor");
        if (!c) {
            invalid_args(conn,
                         "screen.capture 'cursor' must be a boolean");
            return;
        }
        include_cursor = *c;
    }

    // --- encoding: enum base64|binary, default base64 (#91) ---------------
    bool encoding_binary = false;
    if (args.present("encoding")) {
        auto e = args.str("encoding");
        if (!e || (*e != "base64" && *e != "binary")) {
            invalid_args(conn,
                         "screen.capture 'encoding' must be 'base64' or "
                         "'binary'");
            return;
        }
        encoding_binary = (*e == "binary");
    }

    // OUTPUT BOUNDARY ------------------------------------------------------
    // `format: webp|jpeg|heic` needs an encoder not bundled in this build.
    // Args are fully parsed + validated above; the not-yet-implemented
    // OUTPUT is surfaced here. unsupported_format is the spec-declared code
    // for a build that cannot produce the requested output form.
    if (!format_has_encoder(format)) {
        conn.writer().write_err(
            ErrorCode::UnsupportedFormat,
            "{\"reason\":\"format encoder not bundled in this build; "
            "advertised in system.info.capabilities.image_formats "
            "(phase2b)\"}");
        return;
    }
#ifndef RH_MODERN
    // `encoding: binary` is the Protocol v3 PR1.d "Shape B" side-channel,
    // scoped to the modern family only. Non-modern families (windows-legacy)
    // keep their existing behaviour: the side-channel is unavailable, so a
    // binary request is surfaced as unsupported_format exactly as before
    // (the only in-x-errors code that fits "this build can't produce that
    // output form"). The modern path is handled at the emission site below.
    if (encoding_binary) {
        conn.writer().write_err(
            ErrorCode::UnsupportedFormat,
            "{\"reason\":\"encoding:binary output side-channel is not "
            "available on this family (phase2b)\"}");
        return;
    }
#endif

    // --- capture dispatch (unchanged resource model) ----------------------
    screen::CapturedFrame frame;
    if (has_window) {
        frame = screen::capture_window(hwnd, include_cursor);
    } else if (has_region) {
        frame = screen::capture_region(rx, ry, rw, rh, include_cursor);
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
                                       state.rect.bottom - state.rect.top,
                                       include_cursor);
    } else {
        frame = screen::capture_virtual_screen(include_cursor);
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

#ifdef RH_MODERN
    if (encoding_binary) {
        // Protocol v3 PR1.d "Shape B" binary side-channel (modern family
        // only). Hand the raw encoded image bytes to the codec as an
        // out-of-band blob; the JSON result object carries width/height/
        // format metadata and the framing layer appends a blob_size field.
        // The default base64 path below is left byte-for-byte unchanged.
        char meta[96];
        const int mn = std::snprintf(
            meta, sizeof(meta),
            "\"width\":%d,\"height\":%d,\"format\":\"%s\"",
            frame.width, frame.height,
            (format == Format::Png) ? "png" : "bmp");
        if (mn <= 0) {
            conn.writer().write_err(
                ErrorCode::NotSupported,
                "{\"reason\":\"metadata format failed\"}");
            return;
        }
        conn.writer().write_ok_blob(
            std::string_view{meta, static_cast<std::size_t>(mn)},
            wire::ByteView{encoded.data(), encoded.size()});
        return;
    }
#endif

    conn.writer().write_ok(wire::ByteView{encoded.data(), encoded.size()});
}

}  // namespace remote_hands::screen_verbs

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

#pragma once

// Standard base64 encoder (RFC 4648) — single shared definition.
//
// Three TUs needed this independently:
//
//   * registry.cpp        — REG_BINARY values on the wire
//   * mcp_session.cpp     — screen.capture default-path PNG bytes in an MCP
//                           image content item (framing §1.6)
//   * vision.cpp          — vision.describe data-URL image payload (R6)
//
// The first two carried byte-identical file-local copies; rather than add a
// third, both call sites consume this header and the new vision.describe path
// joins them. Header-only `inline` so each TU links its own copy (no extra
// .cpp / link order).
//
// Two overloads are provided so callers can pass the byte type they already
// hold without a cast — `const unsigned char*` / `const std::byte*` / their
// signed-char equivalents — but ALL paths funnel through the same core
// implementation (encode_impl). The encoding is byte-identical to the
// pre-promotion file-local copies; the algorithm was copied verbatim from
// registry.cpp (which was the reference for mcp_session.cpp's copy too).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace remote_hands {

namespace base64_detail {

inline std::string encode_impl(const unsigned char* data, std::size_t len) {
    static constexpr char kTbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i])     << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) <<  8) |
                                 static_cast<std::uint32_t>(data[i + 2]);
        out += kTbl[(n >> 18) & 0x3f];
        out += kTbl[(n >> 12) & 0x3f];
        out += kTbl[(n >>  6) & 0x3f];
        out += kTbl[ n        & 0x3f];
    }
    const std::size_t rem = len - i;
    if (rem == 1) {
        const std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
        out += kTbl[(n >> 18) & 0x3f];
        out += kTbl[(n >> 12) & 0x3f];
        out += '=';
        out += '=';
    } else if (rem == 2) {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i])     << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) <<  8);
        out += kTbl[(n >> 18) & 0x3f];
        out += kTbl[(n >> 12) & 0x3f];
        out += kTbl[(n >>  6) & 0x3f];
        out += '=';
    }
    return out;
}

}  // namespace base64_detail

// Primary signature: byte buffer + length. std::byte matches screen_capture
// pixels / image::encode_png output naturally.
inline std::string base64_encode(const std::byte* data, std::size_t len) {
    return base64_detail::encode_impl(
        reinterpret_cast<const unsigned char*>(data), len);
}

inline std::string base64_encode(const unsigned char* data, std::size_t len) {
    return base64_detail::encode_impl(data, len);
}

// `char*` overload for legacy call sites (registry/mcp framing) that hold the
// payload as a `char` or `BYTE` buffer. `BYTE` (Windows) is a typedef for
// `unsigned char`, which goes through the overload above; `char` and signed
// `char` route here.
inline std::string base64_encode(const char* data, std::size_t len) {
    return base64_detail::encode_impl(
        reinterpret_cast<const unsigned char*>(data), len);
}

// ---------------------------------------------------------------------------
// Standard base64 DECODE (RFC 4648, std alphabet `A-Za-z0-9+/`).
//
// Added by R7 (vision.calibrate) — the caller-supplied image arrives as a
// base64 string and both `calibrate` and any future caller-bytes paths need
// a strict decoder that:
//
//   * accepts ASCII whitespace anywhere in the input (tabs, spaces, CR, LF —
//     matches the leniency real JSON-over-the-wire payloads often acquire
//     when an LLM client wraps a long line),
//   * accepts the standard alphabet only (no URL-safe `-_` variant — callers
//     already produce standard base64 via every existing tool path),
//   * accepts both padded (`=`/`==`) and unpadded inputs (RFC 4648 §3.2
//     allows either; some clients omit padding),
//   * STRICTLY rejects every other byte — including NUL and control bytes —
//     with std::nullopt, so a malformed payload surfaces as the verb's own
//     invalid_args rather than silently producing partial bytes.
//
// Returns std::nullopt for:
//   * any byte that is not [A-Za-z0-9+/=] or ASCII whitespace,
//   * a residue of exactly one base64 character (1 char = 6 bits — cannot
//     represent any whole byte; this is the canonical "malformed trailer"
//     signal in every conforming decoder).
//
// Empty input -> Some(empty vector). The verb layer checks for empty-decoded
// separately when that's semantically meaningful.
inline std::optional<std::vector<std::byte>> base64_decode(std::string_view s) {
    // Inverse table: invalid -> 0xFF, '=' -> 0xFE, whitespace -> 0xFD,
    // valid base64 char -> its 6-bit value (0..63).
    // Built once via Meyers-singleton lambda — no constexpr-lambda dependency
    // (works on every C++17 compiler this repo targets).
    struct InvTable {
        unsigned char t[256];
        InvTable() {
            for (int i = 0; i < 256; ++i) t[i] = 0xFF;
            const char alpha[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                "0123456789+/";
            for (int i = 0; i < 64; ++i) {
                t[static_cast<unsigned char>(alpha[i])] =
                    static_cast<unsigned char>(i);
            }
            t[static_cast<unsigned char>('=')] = 0xFE;
            // ASCII whitespace tolerated: space, tab, LF, CR, VT, FF.
            t[static_cast<unsigned char>(' ' )] = 0xFD;
            t[static_cast<unsigned char>('\t')] = 0xFD;
            t[static_cast<unsigned char>('\n')] = 0xFD;
            t[static_cast<unsigned char>('\r')] = 0xFD;
            t[static_cast<unsigned char>('\v')] = 0xFD;
            t[static_cast<unsigned char>('\f')] = 0xFD;
        }
    };
    static const InvTable kTable;
    const unsigned char* kInv = kTable.t;

    std::vector<std::byte> out;
    out.reserve((s.size() / 4) * 3 + 2);
    std::uint32_t acc = 0;
    int          bits = 0;
    bool         seen_pad = false;

    for (unsigned char c : s) {
        const unsigned char v = kInv[c];
        if (v == 0xFD) continue;            // whitespace
        if (v == 0xFE) { seen_pad = true; continue; }  // '=' — stop accumulating
        if (v == 0xFF) return std::nullopt; // any other byte (NUL, control,
                                            // '-', '_', non-ASCII, etc.)
        if (seen_pad) return std::nullopt;  // alphabet byte after a '=' is
                                            // malformed.
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((acc >> bits) & 0xFF));
            acc &= (1u << bits) - 1u;
        }
    }

    // Residue analysis: after consuming the input, the accumulator must hold
    // 0, 2, or 4 unconsumed bits (i.e. group sizes of 4, 2 or 3 base64 chars).
    // A residue of 6 bits = exactly one trailing char = cannot encode any
    // byte — malformed. Per RFC 4648 §3.5, decoded payload bits beyond the
    // final byte boundary must be zero; we accept non-canonical encodings
    // (some encoders emit them) to be liberal but the residue COUNT must be
    // valid.
    if (bits == 6) return std::nullopt;
    return out;
}

}  // namespace remote_hands

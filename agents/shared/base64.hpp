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
#include <string>

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

}  // namespace remote_hands

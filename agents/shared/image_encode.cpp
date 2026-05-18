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

#include "image_encode.hpp"

#include "platform.hpp"

#include <cstring>

namespace remote_hands::image {

namespace {

void write_u16_le(std::byte* p, std::uint16_t v) {
    p[0] = static_cast<std::byte>(v        & 0xFF);
    p[1] = static_cast<std::byte>((v >> 8) & 0xFF);
}

void write_u32_le(std::byte* p, std::uint32_t v) {
    p[0] = static_cast<std::byte>(v         & 0xFF);
    p[1] = static_cast<std::byte>((v >> 8 ) & 0xFF);
    p[2] = static_cast<std::byte>((v >> 16) & 0xFF);
    p[3] = static_cast<std::byte>((v >> 24) & 0xFF);
}

void write_i32_le(std::byte* p, std::int32_t v) {
    write_u32_le(p, static_cast<std::uint32_t>(v));
}

}  // namespace

// ---------------------------------------------------------------------------
// BMP — 32-bpp, top-down. Hand-rolled; smaller than pulling in WIC.

std::vector<std::byte> encode_bmp(const screen::CapturedFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.pixels.empty()) {
        return {};
    }

    constexpr std::size_t kFileHeaderBytes = 14;
    constexpr std::size_t kInfoHeaderBytes = 40;
    constexpr std::size_t kPixelOffset     = kFileHeaderBytes + kInfoHeaderBytes;

    const std::size_t pixel_bytes =
        static_cast<std::size_t>(frame.width) *
        static_cast<std::size_t>(frame.height) * 4;
    const std::size_t total = kPixelOffset + pixel_bytes;

    std::vector<std::byte> out(total);
    auto* p = out.data();

    // BITMAPFILEHEADER
    p[0] = std::byte{'B'};
    p[1] = std::byte{'M'};
    write_u32_le(p + 2,  static_cast<std::uint32_t>(total));
    write_u16_le(p + 6,  0);                                    // reserved1
    write_u16_le(p + 8,  0);                                    // reserved2
    write_u32_le(p + 10, static_cast<std::uint32_t>(kPixelOffset));

    // BITMAPINFOHEADER
    auto* h = p + kFileHeaderBytes;
    write_u32_le(h + 0,  static_cast<std::uint32_t>(kInfoHeaderBytes));
    write_i32_le(h + 4,  frame.width);
    write_i32_le(h + 8,  -frame.height);                        // negative = top-down
    write_u16_le(h + 12, 1);                                    // planes
    write_u16_le(h + 14, 32);                                   // bits per pixel
    write_u32_le(h + 16, 0);                                    // BI_RGB
    write_u32_le(h + 20, 0);                                    // image size
    write_u32_le(h + 24, 2835);                                 // x ppm (~72 dpi)
    write_u32_le(h + 28, 2835);                                 // y ppm
    write_u32_le(h + 32, 0);                                    // colours used
    write_u32_le(h + 36, 0);                                    // important colours

    std::memcpy(p + kPixelOffset, frame.pixels.data(), pixel_bytes);
    return out;
}

// ---------------------------------------------------------------------------
// PNG — delegated to the platform layer (WIC on modern, GDI+ on legacy).

std::vector<std::byte> encode_png(const screen::CapturedFrame& frame) {
    return platform::encode_png(frame);
}

}  // namespace remote_hands::image

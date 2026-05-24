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

// Image encoders for `screen.*` and (future) `watch.region`.
//
// Input is the BGRA8888 top-down representation produced by
// screen_capture.hpp. PNG encoding goes via the Windows Imaging Component
// (WIC), which is built into Windows and requires no third-party deps. BMP
// encoding is a small hand-rolled writer for cases where the caller wants
// the absolute-cheapest, parser-free format.
//
// WebP support is deferred until libwebp is bundled.

#include "screen_capture.hpp"

#include <cstddef>
#include <vector>

namespace remote_hands::image {

// Returns the bytes of a 32-bpp top-down BMP representation of `frame`.
// Always succeeds (or returns an empty vector on impossible input).
std::vector<std::byte> encode_bmp(const screen::CapturedFrame& frame);

// Returns the bytes of a PNG via WIC. Returns an empty vector on COM
// failure. Caller must have a COM apartment initialised on the current
// thread (Connection threads do).
std::vector<std::byte> encode_png(const screen::CapturedFrame& frame);

// ---------------------------------------------------------------------------
// Clipboard DIB ↔ PNG helpers (CF_DIB / CF_DIBV5 transcode for clipboard.*).
//
// These are independent of the screen.capture encoder path above. They go via
// the Windows Imaging Component (WIC), which is available on both modern and
// legacy targets (windowscodecs.dll ships with Windows XP SP2 + redist;
// standard on Vista+). Each function returns true on success and false on
// COM/decoder failure (caller maps to an ARH error code).
//
// `dib_data` points to a packed CF_DIB block: a BITMAPINFOHEADER (or
// BITMAPV5HEADER), optional colour table (palette / bitfields masks), then
// pixel data — exactly the layout the clipboard hands back from
// GetClipboardData(CF_DIB) / (CF_DIBV5). `dib_to_png` reads the header,
// computes the pixel offset, asks WIC to decode it via a CreateDecoderFromStream
// path with a synthesised BMP file header, then re-encodes the resulting
// frame as a PNG.
//
// `png_to_dib` takes a PNG byte stream, decodes it via WIC, converts the
// frame to 32bpp BGRA, and writes a CF_DIB block (BITMAPINFOHEADER + pixels;
// no BITMAPFILEHEADER — that's only used for on-disk BMP files, never for
// the clipboard) suitable for handing straight to SetClipboardData(CF_DIB).
//
// `width_out` / `height_out` are filled with the decoded image dimensions
// (positive integers; negative heights from top-down DIBs are returned as
// their absolute value so callers don't have to re-normalise).

bool dib_to_png(const void* dib_data, std::size_t dib_size,
                std::vector<std::byte>& out_png,
                int& width_out, int& height_out);

bool png_to_dib(const std::byte* png_data, std::size_t png_size,
                std::vector<std::byte>& out_dib);

}  // namespace remote_hands::image

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

}  // namespace remote_hands::image

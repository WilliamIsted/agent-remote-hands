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

// Screen capture primitives — BitBlt-based today, with WGC envisioned as a
// future runtime-detected fast path.
//
// Returned frames carry top-down BGRA8888 pixels (one byte each for B, G, R,
// A). Unused alpha channel is set to 0xFF. The encoder layer
// (`image_encode.hpp`) consumes this representation.

#include <cstddef>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace remote_hands::screen {

struct CapturedFrame {
    int                     width  = 0;
    int                     height = 0;
    std::vector<std::byte>  pixels;   // BGRA, top-down, no row padding (stride = width*4)
};

// Captures the full virtual screen (multi-monitor span).
CapturedFrame capture_virtual_screen();

// Captures a screen-coordinate region. Clipped to the virtual screen bounds.
// Returns an empty frame on failure.
CapturedFrame capture_region(int x, int y, int w, int h);

// Captures a top-level window using PrintWindow (DWM-composited content
// included). Returns an empty frame on failure.
CapturedFrame capture_window(HWND hwnd);

}  // namespace remote_hands::screen

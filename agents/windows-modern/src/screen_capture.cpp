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

#include "screen_capture.hpp"

#include <algorithm>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace remote_hands::screen {

namespace {

// RAII for HDCs and HBITMAPs.
struct DcDeleter {
    HDC dc = nullptr;
    bool release_to_window = false;
    HWND owner = nullptr;
    ~DcDeleter() {
        if (!dc) return;
        if (release_to_window) ReleaseDC(owner, dc);
        else                   DeleteDC(dc);
    }
};

struct BitmapDeleter {
    HBITMAP h = nullptr;
    ~BitmapDeleter() { if (h) DeleteObject(h); }
};

// Composite the Win32 mouse cursor onto a captured bitmap.
//
// The OS cursor is drawn by the system's separate cursor compositor on top of
// the desktop *after* the GDI device context that BitBlt/PrintWindow captures
// from. So neither primitive includes the cursor by default. To put it in the
// image, we explicitly retrieve it via GetCursorInfo + GetIconInfo and draw
// it onto the captured bitmap's HDC using DrawIconEx, offset by the cursor's
// hotspot and translated from screen coords to bitmap coords via the bitmap's
// origin in screen space.
//
// `bmp_origin_x/y` is the screen-space coordinate of the bitmap's (0,0).
// For capture_region(x,y,w,h) that's (x,y); for capture_window it's
// GetWindowRect's top-left; for capture_virtual_screen it's the virtual-screen
// origin from SM_X/YVIRTUALSCREEN.
//
// No-ops if the cursor is hidden (CURSOR_SHOWING flag clear), if the cursor
// info or icon info can't be retrieved, or if the cursor's logical position
// falls outside the bitmap (DrawIconEx will clip but we save the syscall).
void overlay_cursor(HDC mem_dc, int bmp_w, int bmp_h,
                    int bmp_origin_x, int bmp_origin_y) {
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci)) return;
    if (!(ci.flags & CURSOR_SHOWING)) return;
    if (!ci.hCursor) return;

    ICONINFO ii{};
    if (!GetIconInfo(ci.hCursor, &ii)) return;

    // The mask/colour bitmaps from GetIconInfo are caller-owned per docs.
    struct BmpGuard {
        HBITMAP h;
        ~BmpGuard() { if (h) DeleteObject(h); }
    } mask_guard{ii.hbmMask}, color_guard{ii.hbmColor};

    const int draw_x = ci.ptScreenPos.x - bmp_origin_x - static_cast<int>(ii.xHotspot);
    const int draw_y = ci.ptScreenPos.y - bmp_origin_y - static_cast<int>(ii.yHotspot);

    // Cheap visibility cull: a cursor sprite is at most ~64x64 (large
    // accessibility cursors can be larger but rarely exceed 128x128).
    constexpr int kCursorMaxSize = 128;
    if (draw_x + kCursorMaxSize < 0 || draw_x > bmp_w ||
        draw_y + kCursorMaxSize < 0 || draw_y > bmp_h) {
        return;
    }

    DrawIconEx(mem_dc, draw_x, draw_y, ci.hCursor,
               0, 0,        // 0 = use cursor's own size
               0, nullptr, DI_NORMAL);
}

CapturedFrame extract_dib(HBITMAP bitmap, HDC mem_dc, int width, int height) {
    CapturedFrame frame;
    frame.width  = width;
    frame.height = height;
    frame.pixels.resize(static_cast<std::size_t>(width) *
                        static_cast<std::size_t>(height) * 4);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = width;
    bi.bmiHeader.biHeight      = -height;       // negative = top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    if (GetDIBits(mem_dc, bitmap, 0, static_cast<UINT>(height),
                  frame.pixels.data(), &bi, DIB_RGB_COLORS) == 0) {
        return {};
    }
    return frame;
}

}  // namespace

CapturedFrame capture_region(int x, int y, int w, int h, bool include_cursor) {
    if (w <= 0 || h <= 0) return {};

    HDC screen_dc = GetDC(nullptr);
    if (!screen_dc) return {};
    DcDeleter screen_guard{screen_dc, true, nullptr};

    HDC mem_dc = CreateCompatibleDC(screen_dc);
    if (!mem_dc) return {};
    DcDeleter mem_guard{mem_dc, false, nullptr};

    HBITMAP bitmap = CreateCompatibleBitmap(screen_dc, w, h);
    if (!bitmap) return {};
    BitmapDeleter bm_guard{bitmap};

    HGDIOBJ prev = SelectObject(mem_dc, bitmap);
    if (!BitBlt(mem_dc, 0, 0, w, h, screen_dc, x, y, SRCCOPY | CAPTUREBLT)) {
        SelectObject(mem_dc, prev);
        return {};
    }

    if (include_cursor) overlay_cursor(mem_dc, w, h, x, y);

    SelectObject(mem_dc, prev);

    return extract_dib(bitmap, mem_dc, w, h);
}

CapturedFrame capture_virtual_screen(bool include_cursor) {
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return capture_region(x, y, w, h, include_cursor);
}

CapturedFrame capture_window(HWND hwnd, bool include_cursor) {
    if (!hwnd || !IsWindow(hwnd)) return {};

    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return {};
    const int w = rc.right  - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return {};

    HDC win_dc = GetDC(hwnd);
    if (!win_dc) return {};
    DcDeleter win_guard{win_dc, true, hwnd};

    HDC mem_dc = CreateCompatibleDC(win_dc);
    if (!mem_dc) return {};
    DcDeleter mem_guard{mem_dc, false, nullptr};

    HBITMAP bitmap = CreateCompatibleBitmap(win_dc, w, h);
    if (!bitmap) return {};
    BitmapDeleter bm_guard{bitmap};

    HGDIOBJ prev = SelectObject(mem_dc, bitmap);

    // PrintWindow with PW_RENDERFULLCONTENT (Win 8.1+) captures DWM-composited
    // content even when the window is occluded. Fall back to a plain
    // PrintWindow on older systems where the flag is ignored.
    constexpr UINT kRenderFullContent = 0x00000002;
    if (!PrintWindow(hwnd, mem_dc, kRenderFullContent)) {
        if (!PrintWindow(hwnd, mem_dc, 0)) {
            SelectObject(mem_dc, prev);
            return {};
        }
    }

    if (include_cursor) overlay_cursor(mem_dc, w, h, rc.left, rc.top);

    SelectObject(mem_dc, prev);

    return extract_dib(bitmap, mem_dc, w, h);
}

}  // namespace remote_hands::screen

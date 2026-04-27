/* agent.cpp — Agent Remote Hands, windows-modern target
 *
 * Modern Windows implementation: Vista / 7 / 8 / 8.1 / 10 / 11.
 *
 * Differs from the windows-nt agent in four places, all motivated by
 * Vista-and-later breaking the assumptions XP made:
 *
 *   1. Input via SendInput (Unicode + scan codes + synthetic distinguishable),
 *      not legacy keybd_event / mouse_event.
 *   2. Capture via BitBlt across the *virtual screen* (multi-monitor) plus
 *      DPI awareness opt-in via manifest. WGC (Windows.Graphics.Capture) is
 *      the eventual answer for hardware-accelerated surfaces; left as a TODO
 *      with a BitBlt fallback that handles ~95% of cases.
 *   3. Service registration via Task Scheduler "at logon" task, not SCM.
 *      SERVICE_INTERACTIVE_PROCESS is dead on Vista+ (Session 0 isolation);
 *      a logon task runs in the user's interactive session.
 *   4. INFO advertises capture=gdi multi_monitor=yes dpi_aware=yes
 *      input=sendinput auto_start=task — so MCP clients can branch correctly.
 *
 * Wire protocol: identical to PROTOCOL.md v1. The same conformance suite
 * grades this binary against windows-nt; passing is the gate.
 *
 * Build: see CMakeLists.txt — modern MSVC + manifest embed.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winsvc.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <ole2.h>
#include <comdef.h>
#include <taskschd.h>
#include <gdiplus.h>     // PNG encode via Gdiplus::Bitmap
#include <webp/encode.h> // WebP encode via libwebp (bundled via FetchContent)
#include "wgc.h"         // Windows.Graphics.Capture wrapper (separate TU)
#include "uia.h"         // UI Automation wrapper (separate TU)
#include "discovery.h"   // mDNS responder (separate TU)
#include <process.h>     // _beginthreadex
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <wchar.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "taskschd.lib")

#define DEFAULT_PORT 8765
#define LINEBUF 8192

/* Same caps as windows-nt — set the boundary for fault tolerance. */
#define MAX_WRITE_BYTES    (256 * 1024 * 1024)
#define MAX_CLIPSET_BYTES   (16 * 1024 * 1024)
#define MAX_RUN_OUTPUT      (16 * 1024 * 1024)
#define MAX_LIST_BYTES      ( 8 * 1024 * 1024)
#define MAX_PS_BYTES        ( 4 * 1024 * 1024)
#define MAX_WINLIST_BYTES   ( 4 * 1024 * 1024)
#define MAX_ENV_BYTES       ( 1 * 1024 * 1024)

#define SERVICE_NAME    "RemoteHands"
#define SERVICE_DISPLAY "Agent Remote Hands"
#define TASK_FOLDER     L"\\"
#define TASK_NAME       L"RemoteHands"

/* Concurrency cap: each connection runs on its own thread so a long-running
   verb (WATCH, RUN, big READ) doesn't block other connections. Reject the
   (N+1)th client with ERR busy. */
#define MAX_CONNECTIONS 4

static int g_port = DEFAULT_PORT;
static int g_enable_power = 0;
static int g_enable_discovery = 0;  /* opt-in via REMOTE_HANDS_DISCOVERABLE=1 */

/* Locks for shared mutable state. Win32 file/clipboard/GDI calls handle
   their own thread safety; only the EXEC/WAIT process table and the live-
   connection counter are agent-managed shared state. */
static CRITICAL_SECTION g_proc_lock;
static CRITICAL_SECTION g_conn_lock;
static int g_active_connections = 0;

/* See windows-nt/agent.c — global ABORT generation. Bumped via
   InterlockedIncrement; long-running verbs check each iteration. */
static volatile LONG g_abort_generation = 0;

/* GDI+ for PNG encoding. Same idea as windows-nt — initialised once at
   startup, shut down at exit. The C++ wrapper is cleaner here than the
   flat C API; we don't need dynamic loading because gdiplus.lib is part
   of every Windows install from XP onwards. */
static ULONG_PTR g_gdiplus_token = 0;
static bool g_png_available = false;

/* ------------------------------------------------------------------ */
/* Network helpers (verbatim from windows-nt agent.c)                 */
/* ------------------------------------------------------------------ */

static int recv_line(SOCKET s, char *buf, int max) {
    int n = 0;
    while (n < max - 1) {
        char c;
        int r = recv(s, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '\n') break;
        if (c == '\r') continue;
        buf[n++] = c;
    }
    buf[n] = 0;
    return n;
}

static int send_all(SOCKET s, const char *buf, int len) {
    while (len > 0) {
        int r = send(s, buf, len, 0);
        if (r <= 0) return -1;
        buf += r;
        len -= r;
    }
    return 0;
}

static int send_str(SOCKET s, const char *str) {
    return send_all(s, str, (int)strlen(str));
}

static int recv_n(SOCKET s, BYTE *buf, int n) {
    while (n > 0) {
        int r = recv(s, (char*)buf, n, 0);
        if (r <= 0) return -1;
        buf += r;
        n -= r;
    }
    return 0;
}

static int send_ok(SOCKET s) { return send_str(s, "OK\n"); }

static int send_err(SOCKET s, const char *msg) {
    char buf[512];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "ERR %s\n", msg);
    return send_str(s, buf);
}

static int send_ok_data(SOCKET s, const BYTE *data, DWORD size) {
    char hdr[64];
    int n = _snprintf_s(hdr, sizeof(hdr), _TRUNCATE, "OK %lu\n", (unsigned long)size);
    if (send_all(s, hdr, n) < 0) return -1;
    if (size > 0) return send_all(s, (const char*)data, (int)size);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Key name --> VK mapping (verbatim — protocol contract)             */
/* ------------------------------------------------------------------ */

static int parse_vk(const char *name) {
    int n;
    char c;
    if (!name || !*name) return -1;
    if (name[0] && !name[1]) {
        c = (char)toupper((unsigned char)name[0]);
        if (c >= '0' && c <= '9') return c;
        if (c >= 'A' && c <= 'Z') return c;
    }
    if ((name[0] == 'f' || name[0] == 'F') && (name[1] >= '0' && name[1] <= '9')) {
        n = atoi(name + 1);
        if (n >= 1 && n <= 24) return VK_F1 + (n - 1);
    }
    if (!_stricmp(name, "enter") || !_stricmp(name, "return")) return VK_RETURN;
    if (!_stricmp(name, "tab")) return VK_TAB;
    if (!_stricmp(name, "esc") || !_stricmp(name, "escape")) return VK_ESCAPE;
    if (!_stricmp(name, "space")) return VK_SPACE;
    if (!_stricmp(name, "bsp") || !_stricmp(name, "backspace")) return VK_BACK;
    if (!_stricmp(name, "del") || !_stricmp(name, "delete")) return VK_DELETE;
    if (!_stricmp(name, "ins") || !_stricmp(name, "insert")) return VK_INSERT;
    if (!_stricmp(name, "home")) return VK_HOME;
    if (!_stricmp(name, "end")) return VK_END;
    if (!_stricmp(name, "pgup") || !_stricmp(name, "pageup")) return VK_PRIOR;
    if (!_stricmp(name, "pgdn") || !_stricmp(name, "pagedown")) return VK_NEXT;
    if (!_stricmp(name, "up")) return VK_UP;
    if (!_stricmp(name, "down")) return VK_DOWN;
    if (!_stricmp(name, "left")) return VK_LEFT;
    if (!_stricmp(name, "right")) return VK_RIGHT;
    if (!_stricmp(name, "win") || !_stricmp(name, "super") || !_stricmp(name, "lwin")) return VK_LWIN;
    if (!_stricmp(name, "rwin")) return VK_RWIN;
    if (!_stricmp(name, "ctrl") || !_stricmp(name, "control")) return VK_CONTROL;
    if (!_stricmp(name, "alt")) return VK_MENU;
    if (!_stricmp(name, "shift")) return VK_SHIFT;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Input via SendInput                                                */
/* ------------------------------------------------------------------ */

static void send_key(WORD vk, DWORD flags) {
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = flags;
    SendInput(1, &in, sizeof(in));
}

static void send_unicode_char(WCHAR ch, DWORD flags) {
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = ch;
    in.ki.dwFlags = KEYEVENTF_UNICODE | flags;
    SendInput(1, &in, sizeof(in));
}

/* Returns 1 on successful injection, 0 if any modifier or the base key
   couldn't be resolved. The dispatcher uses this to ERR rather than
   silently no-op on `KEY <bad>`. */
static int press_key_combo(const char *spec) {
    int mods[8];
    int n_mods = 0;
    char base[64];
    const char *p = spec;
    int base_vk;
    while (1) {
        const char *sep = strpbrk(p, "-+");
        char tok[16];
        size_t len;
        int vk;
        if (!sep) break;
        len = (size_t)(sep - p);
        if (len >= sizeof(tok)) return 0;
        memcpy(tok, p, len);
        tok[len] = 0;
        vk = parse_vk(tok);
        if (vk < 0) return 0;
        if (n_mods >= 7) return 0;
        mods[n_mods++] = vk;
        p = sep + 1;
    }
    strncpy_s(base, sizeof(base), p, _TRUNCATE);
    base_vk = parse_vk(base);
    if (base_vk < 0) return 0;
    for (int i = 0; i < n_mods; i++) send_key((WORD)mods[i], 0);
    send_key((WORD)base_vk, 0);
    Sleep(20);
    send_key((WORD)base_vk, KEYEVENTF_KEYUP);
    for (int i = n_mods - 1; i >= 0; i--) send_key((WORD)mods[i], KEYEVENTF_KEYUP);
    return 1;
}

static void type_string(const char *str) {
    /* Wire is Latin-1; convert to UTF-16 and inject as Unicode events. This
       is the big win over keybd_event: SendInput with KEYEVENTF_UNICODE
       handles every Basic Multilingual Plane char without going through the
       active keyboard layout. */
    while (*str) {
        unsigned char b = (unsigned char)*str;
        WCHAR wc = (WCHAR)b;  /* Latin-1 -> U+00xx */
        send_unicode_char(wc, 0);
        send_unicode_char(wc, KEYEVENTF_KEYUP);
        Sleep(2);
        str++;
    }
}

/* ------------------------------------------------------------------ */
/* Mouse via SendInput                                                */
/* ------------------------------------------------------------------ */

static void mouse_button(int button, DWORD flag_kind /* DOWN/UP */) {
    DWORD flag = 0;
    if (button == 2) flag = (flag_kind == 0) ? MOUSEEVENTF_RIGHTDOWN  : MOUSEEVENTF_RIGHTUP;
    else if (button == 3) flag = (flag_kind == 0) ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    else flag = (flag_kind == 0) ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flag;
    SendInput(1, &in, sizeof(in));
}

static void do_click(int button, int double_click) {
    mouse_button(button, 0);
    Sleep(20);
    mouse_button(button, 1);
    if (double_click) {
        Sleep(50);
        mouse_button(button, 0);
        Sleep(20);
        mouse_button(button, 1);
    }
}

static void do_mdown(int button) { mouse_button(button, 0); }
static void do_mup(int button)   { mouse_button(button, 1); }

static void do_wheel(int delta) {
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = (DWORD)delta;
    SendInput(1, &in, sizeof(in));
}

static int parse_button(const char *name) {
    if (!name || !*name) return 1;
    if (!_stricmp(name, "left")   || !_stricmp(name, "1")) return 1;
    if (!_stricmp(name, "right")  || !_stricmp(name, "2")) return 2;
    if (!_stricmp(name, "middle") || !_stricmp(name, "3")) return 3;
    return 1;
}

static void cleanup_input_state(void) {
    send_key(VK_SHIFT,   KEYEVENTF_KEYUP);
    send_key(VK_CONTROL, KEYEVENTF_KEYUP);
    send_key(VK_MENU,    KEYEVENTF_KEYUP);
    send_key(VK_LWIN,    KEYEVENTF_KEYUP);
    send_key(VK_RWIN,    KEYEVENTF_KEYUP);
    mouse_button(1, 1);
    mouse_button(2, 1);
    mouse_button(3, 1);
}

/* ------------------------------------------------------------------ */
/* Screen metrics — virtual screen for multi-monitor                   */
/* ------------------------------------------------------------------ */

static void virtual_screen_rect(int *x, int *y, int *w, int *h) {
    *x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    *y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    *w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    *h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

/* ------------------------------------------------------------------ */
/* Screenshot — BitBlt across virtual screen, with cursor compositing  */
/*                                                                    */
/* TODO(wgc): swap to Windows.Graphics.Capture for hardware-accelerated/ */
/* secure surfaces. BitBlt returns black for those (DRM video, modern  */
/* WinUI 3 surfaces, full-screen Direct3D). For now this captures the   */
/* full virtual screen — every monitor — at physical resolution thanks  */
/* to the per-monitor V2 DPI manifest.                                 */
/* ------------------------------------------------------------------ */

/* Image-format token: bmp (default), png, or webp. Negotiated via the
   leading token of the capture verb's args; see parse_image_format().
   WebP carries a quality value in the same struct because the wire syntax
   `webp:NN` lets clients pick lossy quality per call. */
#define IMG_BMP  0
#define IMG_PNG  1
#define IMG_WEBP 2

struct CaptureFormat {
    int kind;     // IMG_BMP | IMG_PNG | IMG_WEBP
    int quality;  // -1 = lossless (BMP/PNG always; WebP "webp" without :NN);
                  //  0..100 = lossy WebP quality from "webp:NN"
};

/* BitBlt fallback: captures into a 24bpp HBITMAP with the OS cursor
   manually composited. Used when WGC is unavailable or for partial rects
   smaller than the full virtual screen. */
static HBITMAP bitblt_screen_to_hbitmap(int sx, int sy, int w, int h) {
    HDC hdc_screen = GetDC(NULL);
    if (!hdc_screen) return NULL;
    HDC hdc_mem = CreateCompatibleDC(hdc_screen);
    HBITMAP hbm = CreateCompatibleBitmap(hdc_screen, w, h);
    HGDIOBJ old = SelectObject(hdc_mem, hbm);
    BitBlt(hdc_mem, 0, 0, w, h, hdc_screen, sx, sy, SRCCOPY);

    CURSORINFO ci;
    ci.cbSize = sizeof(ci);
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor) {
        ICONINFO ii;
        int hx = 0, hy = 0;
        if (GetIconInfo(ci.hCursor, &ii)) {
            hx = (int)ii.xHotspot; hy = (int)ii.yHotspot;
            if (ii.hbmMask)  DeleteObject(ii.hbmMask);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
        }
        DrawIcon(hdc_mem, ci.ptScreenPos.x - hx - sx, ci.ptScreenPos.y - hy - sy, ci.hCursor);
    }

    SelectObject(hdc_mem, old);
    DeleteDC(hdc_mem);
    ReleaseDC(NULL, hdc_screen);
    return hbm;
}

/* Captures the requested rect into an HBITMAP. WGC is preferred at every
   tier — it captures DirectComposition / Direct3D / DRM-protected surfaces
   correctly where BitBlt returns black:
     - rect == full virtual screen → WGC across all monitors, stitched
     - rect within a single monitor  → WGC that monitor, crop client-side
     - everything else (cross-monitor partial, WGC unavailable, errors)
       → BitBlt fallback. */
static HBITMAP capture_screen_to_hbitmap(int sx, int sy, int w, int h) {
    if (wgc_available()) {
        int vsx, vsy, vsw, vsh;
        virtual_screen_rect(&vsx, &vsy, &vsw, &vsh);
        if (sx == vsx && sy == vsy && w == vsw && h == vsh) {
            if (HBITMAP hbm = wgc_capture_virtual_screen()) return hbm;
        } else {
            if (HBITMAP hbm = wgc_capture_rect(sx, sy, w, h)) return hbm;
        }
    }
    return bitblt_screen_to_hbitmap(sx, sy, w, h);
}

/* Per-window capture preferring WGC when available (DirectComposition,
   acrylic, Win11 modern shells render correctly). Falls back to a BitBlt
   of the window's screen rect on failure. */
static HBITMAP capture_window_to_hbitmap(HWND hwnd, RECT const &r) {
    if (wgc_available()) {
        HBITMAP hbm = wgc_capture_window(hwnd);
        if (hbm) return hbm;
    }
    return bitblt_screen_to_hbitmap(r.left, r.top, r.right - r.left, r.bottom - r.top);
}

/* RAII guard for a streaming WGC session — used inside WATCH/WAITFOR so
   the session is closed on every exit path (normal end, abort, send
   failure, capture failure) without duplicating cleanup code. */
struct wgc_session_guard {
    wgc_session *s = nullptr;
    wgc_session_guard() {
        if (wgc_available()) s = wgc_session_open_primary();
    }
    ~wgc_session_guard() { if (s) wgc_session_close(s); }
    wgc_session_guard(wgc_session_guard const &) = delete;
    wgc_session_guard &operator=(wgc_session_guard const &) = delete;

    /* Try the session; fall back to a BitBlt of the virtual screen on
       failure. Returns an HBITMAP the caller must DeleteObject. */
    HBITMAP capture_frame() {
        if (s) {
            if (HBITMAP h = wgc_session_get_frame(s)) return h;
        }
        int x, y, w, h;
        virtual_screen_rect(&x, &y, &w, &h);
        return bitblt_screen_to_hbitmap(x, y, w, h);
    }
};

static int hbitmap_to_bmp(HBITMAP hbm, int w, int h, BYTE **out_buf, DWORD *out_size) {
    HDC hdc = GetDC(NULL);
    if (!hdc) return -1;
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    int row = ((w * 3 + 3) & ~3);
    DWORD px_size = (DWORD)row * (DWORD)h;
    DWORD total = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + px_size;
    BYTE *buf = (BYTE*)malloc(total);
    if (!buf) { ReleaseDC(NULL, hdc); return -1; }
    BITMAPFILEHEADER *bfh = (BITMAPFILEHEADER*)buf;
    bfh->bfType = 0x4D42;
    bfh->bfSize = total;
    bfh->bfReserved1 = 0;
    bfh->bfReserved2 = 0;
    bfh->bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    memcpy(buf + sizeof(BITMAPFILEHEADER), &bi.bmiHeader, sizeof(BITMAPINFOHEADER));
    bi.bmiHeader.biHeight = -h;
    GetDIBits(hdc, hbm, 0, h, buf + bfh->bfOffBits, &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, hdc);
    *out_buf = buf;
    *out_size = total;
    return 0;
}

static int hbitmap_to_png(HBITMAP hbm, BYTE **out_buf, DWORD *out_size) {
    static const CLSID PNG_CLSID =
        { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
    if (!g_png_available) return -2;
    Gdiplus::Bitmap *bmp = Gdiplus::Bitmap::FromHBITMAP(hbm, NULL);
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) {
        delete bmp;
        return -1;
    }
    IStream *stream = NULL;
    if (CreateStreamOnHGlobal(NULL, TRUE, &stream) != S_OK || !stream) {
        delete bmp;
        return -1;
    }
    Gdiplus::Status status = bmp->Save(stream, &PNG_CLSID, NULL);
    delete bmp;
    if (status != Gdiplus::Ok) {
        stream->Release();
        return -1;
    }
    HGLOBAL hg = NULL;
    GetHGlobalFromStream(stream, &hg);
    SIZE_T size = GlobalSize(hg);
    BYTE *src = (BYTE*)GlobalLock(hg);
    BYTE *out = (BYTE*)malloc(size ? size : 1);
    if (!out) { GlobalUnlock(hg); stream->Release(); return -1; }
    if (size) memcpy(out, src, size);
    GlobalUnlock(hg);
    stream->Release();
    *out_buf = out;
    *out_size = (DWORD)size;
    return 0;
}

/* Encodes an HBITMAP to WebP via libwebp. quality < 0 = lossless
   (WebPEncodeLosslessBGR), 0..100 = lossy (WebPEncodeBGR). The DIB we
   receive is BGR, top-down, with row stride 4-byte-aligned. libwebp's
   `*BGR` variants accept that layout natively. */
static int hbitmap_to_webp(HBITMAP hbm, int w, int h, int quality,
                           BYTE **out_buf, DWORD *out_size) {
    HDC hdc = GetDC(NULL);
    if (!hdc) return -1;
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    int row_bytes = ((w * 3 + 3) & ~3);
    DWORD pixel_size = (DWORD)row_bytes * (DWORD)h;
    BYTE *bgr = (BYTE*)malloc(pixel_size);
    if (!bgr) { ReleaseDC(NULL, hdc); return -1; }
    GetDIBits(hdc, hbm, 0, h, bgr, &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, hdc);

    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    if (quality < 0) {
        encoded_size = WebPEncodeLosslessBGR(bgr, w, h, row_bytes, &encoded);
    } else {
        encoded_size = WebPEncodeBGR(bgr, w, h, row_bytes, (float)quality, &encoded);
    }
    free(bgr);

    if (encoded_size == 0 || !encoded) return -1;

    /* libwebp returns memory it owns; copy into our heap and free theirs
       so the caller can use plain free(). */
    BYTE *result = (BYTE*)malloc(encoded_size);
    if (!result) { WebPFree(encoded); return -1; }
    memcpy(result, encoded, encoded_size);
    WebPFree(encoded);

    *out_buf = result;
    *out_size = (DWORD)encoded_size;
    return 0;
}

/* Encode an HBITMAP per the requested format. Caller already holds the
   bitmap from whatever capture path (BitBlt screen, BitBlt rect, WGC
   monitor, WGC window) — this just serializes it. */
static int hbitmap_to_format(HBITMAP hbm, int w, int h, CaptureFormat fmt,
                             BYTE **out_buf, DWORD *out_size) {
    switch (fmt.kind) {
        case IMG_PNG:  return hbitmap_to_png(hbm, out_buf, out_size);
        case IMG_WEBP: return hbitmap_to_webp(hbm, w, h, fmt.quality, out_buf, out_size);
        default:       return hbitmap_to_bmp(hbm, w, h, out_buf, out_size);
    }
}

static int capture_image_rect(CaptureFormat format, int sx, int sy, int w, int h,
                              BYTE **out_buf, DWORD *out_size) {
    HBITMAP hbm = capture_screen_to_hbitmap(sx, sy, w, h);
    if (!hbm) return -1;
    int rc = hbitmap_to_format(hbm, w, h, format, out_buf, out_size);
    DeleteObject(hbm);
    return rc;
}

static int capture_image(CaptureFormat format, BYTE **out_buf, DWORD *out_size) {
    int x, y, w, h;
    virtual_screen_rect(&x, &y, &w, &h);
    return capture_image_rect(format, x, y, w, h, out_buf, out_size);
}

/* Parses an optional leading format token. Recognised tokens:
     bmp       → { IMG_BMP, -1 }
     png       → { IMG_PNG, -1 }
     webp      → { IMG_WEBP, -1 }   (lossless)
     webp:NN   → { IMG_WEBP, NN }   (lossy, NN ∈ 0..100)
   Anything else is left untouched and the default { IMG_BMP, -1 } is
   returned. The caller advances past the token; absent token = default. */
static CaptureFormat parse_image_format(char **arg_p) {
    CaptureFormat fmt = { IMG_BMP, -1 };
    char *arg = *arg_p;
    int consume = 0;

    if (_strnicmp(arg, "bmp", 3) == 0 && (arg[3] == 0 || arg[3] == ' ')) {
        fmt.kind = IMG_BMP; consume = 3;
    } else if (_strnicmp(arg, "png", 3) == 0 && (arg[3] == 0 || arg[3] == ' ')) {
        fmt.kind = IMG_PNG; consume = 3;
    } else if (_strnicmp(arg, "webp", 4) == 0) {
        int p = 4;
        int quality = -1;
        if (arg[p] == ':') {
            p++;
            int n = 0;
            int found = 0;
            while (arg[p] >= '0' && arg[p] <= '9') {
                n = n * 10 + (arg[p] - '0');
                p++;
                found = 1;
            }
            if (!found || n > 100) return fmt;  // malformed → default BMP
            quality = n;
        }
        if (arg[p] != 0 && arg[p] != ' ') return fmt;  // trailing garbage → default
        fmt.kind = IMG_WEBP;
        fmt.quality = quality;
        consume = p;
    }
    if (consume) {
        *arg_p = arg + consume;
        while (**arg_p == ' ') (*arg_p)++;
    }
    return fmt;
}

/* ------------------------------------------------------------------ */
/* Change-detection thumbnail (used by WATCH / WAITFOR)               */
/* ------------------------------------------------------------------ */

#define THUMB_SIZE 32
#define THUMB_BYTES (THUMB_SIZE * THUMB_SIZE * 3)

static int capture_thumbnail(BYTE *out_thumb) {
    /* Mirrors windows-nt/agent.c — see that file for rationale. Uses the
       virtual screen so multi-monitor setups produce a thumbnail covering
       all displays. Cursor not composited; cursor movement won't trigger
       a WATCH frame. */
    HDC hdc_screen = GetDC(NULL);
    if (!hdc_screen) return -1;
    int sx, sy, sw, sh;
    virtual_screen_rect(&sx, &sy, &sw, &sh);
    HDC hdc_mem = CreateCompatibleDC(hdc_screen);
    HBITMAP hbm = CreateCompatibleBitmap(hdc_screen, THUMB_SIZE, THUMB_SIZE);
    HGDIOBJ old = SelectObject(hdc_mem, hbm);
    SetStretchBltMode(hdc_mem, HALFTONE);
    SetBrushOrgEx(hdc_mem, 0, 0, NULL);
    StretchBlt(hdc_mem, 0, 0, THUMB_SIZE, THUMB_SIZE,
               hdc_screen, sx, sy, sw, sh, SRCCOPY);
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = THUMB_SIZE;
    bi.bmiHeader.biHeight = -THUMB_SIZE;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    GetDIBits(hdc_mem, hbm, 0, THUMB_SIZE, out_thumb, &bi, DIB_RGB_COLORS);
    SelectObject(hdc_mem, old);
    DeleteObject(hbm);
    DeleteDC(hdc_mem);
    ReleaseDC(NULL, hdc_screen);
    return 0;
}

static double thumbnail_diff_pct(const BYTE *a, const BYTE *b) {
    int changed = 0;
    for (int i = 0; i < THUMB_SIZE * THUMB_SIZE; i++) {
        int dr = (int)a[i*3+0] - (int)b[i*3+0];
        int dg = (int)a[i*3+1] - (int)b[i*3+1];
        int db = (int)a[i*3+2] - (int)b[i*3+2];
        if (dr < 0) dr = -dr;
        if (dg < 0) dg = -dg;
        if (db < 0) db = -db;
        if (dr > 8 || dg > 8 || db > 8) changed++;
    }
    return (double)changed / (double)(THUMB_SIZE * THUMB_SIZE) * 100.0;
}

/* ------------------------------------------------------------------ */
/* File I/O — same as windows-nt; modern SDK has every API natively   */
/* ------------------------------------------------------------------ */

static int read_whole_file(const char *path, BYTE **out_buf, DWORD *out_size) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD sz = GetFileSize(h, NULL);
    DWORD got = 0;
    BYTE *buf = (BYTE*)malloc(sz ? sz : 1);
    if (!buf) { CloseHandle(h); return -1; }
    if (sz > 0 && !ReadFile(h, buf, sz, &got, NULL)) {
        CloseHandle(h); free(buf); return -1;
    }
    CloseHandle(h);
    *out_buf = buf;
    *out_size = got;
    return 0;
}

static int write_whole_file(const char *path, const BYTE *buf, DWORD size) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, buf, size, &wrote, NULL);
    CloseHandle(h);
    return (ok && wrote == size) ? 0 : -1;
}

static long filetime_to_unix(const FILETIME *ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    return (long)((__int64)(u.QuadPart / 10000000ULL) - 11644473600LL);
}

static int list_dir(const char *path, BYTE **out_buf, DWORD *out_size) {
    char pattern[MAX_PATH];
    int n = _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*", path);
    if (n < 0) return -1;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    size_t cap = 4096, len = 0;
    char *buf = (char*)malloc(cap);
    if (!buf) { FindClose(h); return -1; }
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char type;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) type = 'D';
        else if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) type = 'L';
        else type = 'F';
        ULARGE_INTEGER sz;
        sz.LowPart = fd.nFileSizeLow;
        sz.HighPart = fd.nFileSizeHigh;
        long mtime = filetime_to_unix(&fd.ftLastWriteTime);
        char line[MAX_PATH + 64];
        int line_len = _snprintf_s(line, sizeof(line), _TRUNCATE,
                                   "%c\t%I64u\t%ld\t%s\n",
                                   type, sz.QuadPart, mtime, fd.cFileName);
        if (line_len < 0) continue;
        if (len + (size_t)line_len > MAX_LIST_BYTES) break;
        if (len + (size_t)line_len > cap) {
            char *nb;
            while (len + (size_t)line_len > cap) cap *= 2;
            nb = (char*)realloc(buf, cap);
            if (!nb) { free(buf); FindClose(h); return -1; }
            buf = nb;
        }
        memcpy(buf + len, line, line_len);
        len += line_len;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    *out_buf = (BYTE*)buf;
    *out_size = (DWORD)len;
    return 0;
}

static int do_stat(const char *path, char *type_out,
                   unsigned __int64 *size_out, long *mtime_out) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return -1;
    if (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) *type_out = 'D';
    else if (fa.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) *type_out = 'L';
    else *type_out = 'F';
    ULARGE_INTEGER u;
    u.LowPart = fa.nFileSizeLow;
    u.HighPart = fa.nFileSizeHigh;
    *size_out = u.QuadPart;
    *mtime_out = filetime_to_unix(&fa.ftLastWriteTime);
    return 0;
}

static int do_delete(const char *path) {
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return -1;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) return RemoveDirectoryA(path) ? 0 : -1;
    return DeleteFileA(path) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Process management                                                 */
/* ------------------------------------------------------------------ */

#define MAX_PROCS 64
static HANDLE g_proc_handles[MAX_PROCS] = {0};
static DWORD  g_proc_pids[MAX_PROCS] = {0};
static int    g_proc_count = 0;

static int do_exec(const char *cmdline, DWORD *out_pid) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char buf[1024];
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    strncpy_s(buf, sizeof(buf), cmdline, _TRUNCATE);
    if (!CreateProcessA(NULL, buf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return -1;
    *out_pid = pi.dwProcessId;
    EnterCriticalSection(&g_proc_lock);
    if (g_proc_count < MAX_PROCS) {
        g_proc_handles[g_proc_count] = pi.hProcess;
        g_proc_pids[g_proc_count] = pi.dwProcessId;
        g_proc_count++;
    } else {
        CloseHandle(pi.hProcess);
    }
    LeaveCriticalSection(&g_proc_lock);
    CloseHandle(pi.hThread);
    return 0;
}

/* Returns 0 on normal exit, -1 on lookup failure, -2 on timeout, -3 on ABORT. */
static int do_wait(DWORD pid, DWORD timeout_ms, DWORD *out_exit) {
    HANDLE h = NULL;
    int slot = -1;
    LONG abort_baseline = g_abort_generation;
    EnterCriticalSection(&g_proc_lock);
    for (int i = 0; i < g_proc_count; i++) {
        if (g_proc_handles[i] && g_proc_pids[i] == pid) {
            h = g_proc_handles[i]; slot = i; break;
        }
    }
    LeaveCriticalSection(&g_proc_lock);
    if (!h) {
        h = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (!h) return -1;
    }
    /* Chunk the wait so ABORT can interrupt and so INFINITE waits are
       cancellable. */
    DWORD start = GetTickCount();
    DWORD total_waited = 0;
    while (true) {
        DWORD chunk = 250;
        if (g_abort_generation != abort_baseline) {
            if (slot < 0) CloseHandle(h);
            return -3;
        }
        if (timeout_ms != INFINITE) {
            if (total_waited >= timeout_ms) {
                if (slot < 0) CloseHandle(h);
                return -2;
            }
            if (timeout_ms - total_waited < chunk) chunk = timeout_ms - total_waited;
        }
        DWORD wr = WaitForSingleObject(h, chunk);
        if (wr == WAIT_OBJECT_0) break;
        total_waited = GetTickCount() - start;
    }
    GetExitCodeProcess(h, out_exit);
    if (slot >= 0) {
        EnterCriticalSection(&g_proc_lock);
        if (g_proc_handles[slot] == h && g_proc_pids[slot] == pid) {
            CloseHandle(h);
            g_proc_handles[slot] = NULL;
            g_proc_pids[slot] = 0;
        } else {
            CloseHandle(h);
        }
        LeaveCriticalSection(&g_proc_lock);
    } else {
        CloseHandle(h);
    }
    return 0;
}

static int do_kill(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) return -1;
    BOOL ok = TerminateProcess(h, 1);
    CloseHandle(h);
    return ok ? 0 : -1;
}

static int list_processes(BYTE **out_buf, DWORD *out_size) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (!Process32First(snap, &pe)) { CloseHandle(snap); return -1; }
    size_t cap = 8192, len = 0;
    char *buf = (char*)malloc(cap);
    if (!buf) { CloseHandle(snap); return -1; }
    do {
        char line[MAX_PATH + 32];
        /* PROCESSENTRY32 (== PROCESSENTRY32A here, since UNICODE isn't
           defined for this build) carries szExeFile as char[]. Use %s, not
           %ws — %ws made the line content empty / garbage and PS output
           length came back zero in the conformance suite. */
        int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "%lu\t%s\n",
                            (unsigned long)pe.th32ProcessID, pe.szExeFile);
        if (n < 0) continue;
        if (len + (size_t)n > MAX_PS_BYTES) break;
        if (len + (size_t)n > cap) {
            char *nb;
            while (len + (size_t)n > cap) cap *= 2;
            nb = (char*)realloc(buf, cap);
            if (!nb) { free(buf); CloseHandle(snap); return -1; }
            buf = nb;
        }
        memcpy(buf + len, line, n);
        len += n;
    } while (Process32Next(snap, &pe));
    CloseHandle(snap);
    *out_buf = (BYTE*)buf;
    *out_size = (DWORD)len;
    return 0;
}

static int run_append(char **buf_p, size_t *len_p, size_t *cap_p,
                      const char *chunk, DWORD got) {
    size_t len = *len_p, cap = *cap_p;
    DWORD write = got;
    if (len >= MAX_RUN_OUTPUT) return 0;
    if (len + write > MAX_RUN_OUTPUT) write = (DWORD)(MAX_RUN_OUTPUT - len);
    if (len + write > cap) {
        while (len + write > cap) cap *= 2;
        char *nb = (char*)realloc(*buf_p, cap);
        if (!nb) return -1;
        *buf_p = nb; *cap_p = cap;
    }
    memcpy(*buf_p + len, chunk, write);
    *len_p = len + write;
    return 0;
}

/* Returns 0 on normal exit, -1 on system failure, -3 on ABORT. */
static int do_run(const char *cmdline, DWORD *out_exit, BYTE **out_buf, DWORD *out_size) {
    /* Abort-aware: poll the pipe via PeekNamedPipe, poll the process via
       WaitForSingleObject(50). Each iteration checks g_abort_generation. */
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE hNul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, &sa,
                              OPEN_EXISTING, 0, NULL);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = hNul;

    char buf[1024];
    strncpy_s(buf, sizeof(buf), cmdline, _TRUNCATE);
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, buf, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
        return -1;
    }
    CloseHandle(wr);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);

    size_t cap = 4096, len = 0;
    char *output = (char*)malloc(cap);
    if (!output) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(rd); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        return -1;
    }
    char chunk[4096];
    LONG abort_baseline = g_abort_generation;
    bool aborted = false;

    while (true) {
        if (g_abort_generation != abort_baseline) { aborted = true; break; }

        DWORD avail = 0;
        if (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            DWORD got = 0;
            DWORD to_read = avail > sizeof(chunk) ? sizeof(chunk) : avail;
            if (ReadFile(rd, chunk, to_read, &got, NULL) && got > 0) {
                if (run_append(&output, &len, &cap, chunk, got) < 0) {
                    TerminateProcess(pi.hProcess, 1);
                    free(output);
                    CloseHandle(rd); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
                    return -1;
                }
            }
            continue;
        }

        DWORD wr_status = WaitForSingleObject(pi.hProcess, 50);
        if (wr_status == WAIT_OBJECT_0) {
            while (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                DWORD got = 0;
                DWORD to_read = avail > sizeof(chunk) ? sizeof(chunk) : avail;
                if (!ReadFile(rd, chunk, to_read, &got, NULL) || got == 0) break;
                if (run_append(&output, &len, &cap, chunk, got) < 0) break;
            }
            break;
        }
    }

    if (aborted) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
        CloseHandle(rd);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        free(output);
        return -3;
    }

    CloseHandle(rd);
    GetExitCodeProcess(pi.hProcess, out_exit);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    *out_buf = (BYTE*)output;
    *out_size = (DWORD)len;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Clipboard                                                          */
/* ------------------------------------------------------------------ */

static int clip_get(BYTE **out_buf, DWORD *out_size) {
    if (!OpenClipboard(NULL)) return -1;
    HANDLE h = GetClipboardData(CF_TEXT);
    if (!h) {
        CloseClipboard();
        char *buf = (char*)malloc(1);
        if (!buf) return -1;
        *out_buf = (BYTE*)buf;
        *out_size = 0;
        return 0;
    }
    char *p = (char*)GlobalLock(h);
    if (!p) { CloseClipboard(); return -1; }
    DWORD len = (DWORD)strlen(p);
    char *buf = (char*)malloc(len ? len : 1);
    if (!buf) { GlobalUnlock(h); CloseClipboard(); return -1; }
    if (len) memcpy(buf, p, len);
    GlobalUnlock(h);
    CloseClipboard();
    *out_buf = (BYTE*)buf;
    *out_size = len;
    return 0;
}

static int clip_set(const BYTE *buf, DWORD len) {
    if (!OpenClipboard(NULL)) return -1;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, len + 1);
    if (!h) { CloseClipboard(); return -1; }
    char *p = (char*)GlobalLock(h);
    if (len) memcpy(p, buf, len);
    p[len] = 0;
    GlobalUnlock(h);
    SetClipboardData(CF_TEXT, h);
    CloseClipboard();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Window helpers                                                     */
/* ------------------------------------------------------------------ */

static HWND find_window_by_title_prefix(const char *title) {
    HWND h = GetTopWindow(NULL);
    int tlen = (int)strlen(title);
    while (h) {
        char buf[256];
        if (GetWindowTextA(h, buf, sizeof(buf)) > 0) {
            if (_strnicmp(buf, title, tlen) == 0 && IsWindowVisible(h)) return h;
        }
        h = GetWindow(h, GW_HWNDNEXT);
    }
    return NULL;
}

struct winlist_ctx {
    char *buf;
    size_t cap;
    size_t len;
    int oom;
};

static BOOL CALLBACK enum_window_cb(HWND h, LPARAM lp) {
    winlist_ctx *ctx = (winlist_ctx*)lp;
    if (!IsWindowVisible(h)) return TRUE;
    char title[256];
    if (GetWindowTextA(h, title, sizeof(title)) <= 0) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r)) return TRUE;
    char line[512];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "%llu\t%ld\t%ld\t%ld\t%ld\t%s\n",
                        (unsigned long long)(ULONG_PTR)h,
                        (long)r.left, (long)r.top,
                        (long)(r.right - r.left), (long)(r.bottom - r.top),
                        title);
    if (n < 0) return TRUE;
    if (ctx->len + (size_t)n > MAX_WINLIST_BYTES) return FALSE;
    if (ctx->len + (size_t)n > ctx->cap) {
        size_t newcap = ctx->cap;
        while (ctx->len + (size_t)n > newcap) newcap *= 2;
        char *nb = (char*)realloc(ctx->buf, newcap);
        if (!nb) { ctx->oom = 1; return FALSE; }
        ctx->buf = nb; ctx->cap = newcap;
    }
    memcpy(ctx->buf + ctx->len, line, n);
    ctx->len += n;
    return TRUE;
}

static int list_windows(BYTE **out_buf, DWORD *out_size) {
    winlist_ctx ctx;
    ctx.buf = (char*)malloc(4096);
    if (!ctx.buf) return -1;
    ctx.cap = 4096; ctx.len = 0; ctx.oom = 0;
    EnumWindows(enum_window_cb, (LPARAM)&ctx);
    if (ctx.oom) { free(ctx.buf); return -1; }
    *out_buf = (BYTE*)ctx.buf;
    *out_size = (DWORD)ctx.len;
    return 0;
}

/* ------------------------------------------------------------------ */
/* System                                                             */
/* ------------------------------------------------------------------ */

static int get_idle_seconds(DWORD *out) {
    LASTINPUTINFO lii;
    lii.cbSize = sizeof(lii);
    if (!GetLastInputInfo(&lii)) return -1;
    *out = (GetTickCount() - lii.dwTime) / 1000;
    return 0;
}

static int do_lock(void) {
    return LockWorkStation() ? 0 : -1;
}

static int list_drives(BYTE **out_buf, DWORD *out_size) {
    char drives[512];
    DWORD got = GetLogicalDriveStringsA(sizeof(drives), drives);
    if (got == 0 || got > sizeof(drives)) return -1;
    size_t cap = 1024, len = 0;
    char *buf = (char*)malloc(cap);
    if (!buf) return -1;
    for (char *p = drives; *p; p += strlen(p) + 1) {
        UINT t = GetDriveTypeA(p);
        const char *type;
        switch (t) {
            case DRIVE_REMOVABLE: type = "removable"; break;
            case DRIVE_FIXED:     type = "fixed";     break;
            case DRIVE_REMOTE:    type = "remote";    break;
            case DRIVE_CDROM:     type = "cdrom";     break;
            case DRIVE_RAMDISK:   type = "ramdisk";   break;
            default:              type = "unknown";   break;
        }
        char line[64];
        int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "%s\t%s\n", p, type);
        if (n < 0) continue;
        if (len + (size_t)n > cap) {
            while (len + (size_t)n > cap) cap *= 2;
            char *nb = (char*)realloc(buf, cap);
            if (!nb) { free(buf); return -1; }
            buf = nb;
        }
        memcpy(buf + len, line, n);
        len += n;
    }
    *out_buf = (BYTE*)buf;
    *out_size = (DWORD)len;
    return 0;
}

static int list_env(BYTE **out_buf, DWORD *out_size) {
    LPCH env = GetEnvironmentStringsA();
    if (!env) return -1;
    size_t cap = 8192, len = 0;
    char *buf = (char*)malloc(cap);
    if (!buf) { FreeEnvironmentStringsA(env); return -1; }
    for (LPCH p = env; *p; p += strlen(p) + 1) {
        if (*p == '=') continue;
        size_t n = strlen(p);
        if (len + n + 1 > MAX_ENV_BYTES) break;
        if (len + n + 1 > cap) {
            while (len + n + 1 > cap) cap *= 2;
            char *nb = (char*)realloc(buf, cap);
            if (!nb) { free(buf); FreeEnvironmentStringsA(env); return -1; }
            buf = nb;
        }
        memcpy(buf + len, p, n);
        len += n;
        buf[len++] = '\n';
    }
    FreeEnvironmentStringsA(env);
    *out_buf = (BYTE*)buf;
    *out_size = (DWORD)len;
    return 0;
}

static int enable_shutdown_privilege(void) {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return -1;
    LUID luid;
    if (!LookupPrivilegeValueA(NULL, SE_SHUTDOWN_NAME, &luid)) {
        CloseHandle(token); return -1;
    }
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL);
    CloseHandle(token);
    return (ok && GetLastError() == ERROR_SUCCESS) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Service mode — Task Scheduler "at logon" task                      */
/*                                                                    */
/* Vista+ Session 0 isolation makes SCM-installed services useless    */
/* for an agent that drives an interactive desktop. A logon-trigger   */
/* scheduled task runs in the user's session, sees the user's desktop,  */
/* and starts automatically when they log in.                         */
/* ------------------------------------------------------------------ */

static int do_install(void) {
    /* COM dance to register a logon-triggered task that runs this binary. */
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return -1;
    if (FAILED(CoInitializeSecurity(NULL, -1, NULL, NULL,
                                    RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                                    RPC_C_IMP_LEVEL_IMPERSONATE,
                                    NULL, 0, NULL))) {
        /* Already initialised in this process — non-fatal. */
    }

    ITaskService *svc = NULL;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER,
                                IID_ITaskService, (void**)&svc))) {
        CoUninitialize(); return -1;
    }
    VARIANT v_empty;
    VariantInit(&v_empty);
    if (FAILED(svc->Connect(v_empty, v_empty, v_empty, v_empty))) {
        svc->Release(); CoUninitialize(); return -1;
    }

    ITaskFolder *folder = NULL;
    BSTR root = SysAllocString(TASK_FOLDER);
    HRESULT hr = svc->GetFolder(root, &folder);
    SysFreeString(root);
    if (FAILED(hr)) { svc->Release(); CoUninitialize(); return -1; }

    ITaskDefinition *task = NULL;
    if (FAILED(svc->NewTask(0, &task))) {
        folder->Release(); svc->Release(); CoUninitialize(); return -1;
    }

    /* Logon trigger. */
    ITriggerCollection *trigs = NULL;
    task->get_Triggers(&trigs);
    ITrigger *trig = NULL;
    trigs->Create(TASK_TRIGGER_LOGON, &trig);
    trig->Release();
    trigs->Release();

    /* Action: run this exe path. */
    IActionCollection *actions = NULL;
    task->get_Actions(&actions);
    IAction *action = NULL;
    actions->Create(TASK_ACTION_EXEC, &action);
    IExecAction *exec = NULL;
    action->QueryInterface(IID_IExecAction, (void**)&exec);
    WCHAR exe_path[MAX_PATH];
    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    BSTR exe_bstr = SysAllocString(exe_path);
    exec->put_Path(exe_bstr);
    SysFreeString(exe_bstr);
    exec->Release();
    action->Release();
    actions->Release();

    /* Settings: don't stop on AC change, allow run on demand, etc. */
    ITaskSettings *settings = NULL;
    task->get_Settings(&settings);
    settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
    settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
    settings->put_RunOnlyIfIdle(VARIANT_FALSE);
    settings->Release();

    /* Register. */
    BSTR name_bstr = SysAllocString(TASK_NAME);
    VARIANT v_null;
    VariantInit(&v_null);
    IRegisteredTask *reg = NULL;
    hr = folder->RegisterTaskDefinition(
        name_bstr, task, TASK_CREATE_OR_UPDATE,
        v_null, v_null, TASK_LOGON_INTERACTIVE_TOKEN,
        v_null, &reg);
    SysFreeString(name_bstr);
    task->Release();
    folder->Release();
    svc->Release();

    if (FAILED(hr)) { CoUninitialize(); return -1; }
    if (reg) reg->Release();
    CoUninitialize();
    return 0;
}

static int do_uninstall(void) {
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return -1;
    ITaskService *svc = NULL;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER,
                                IID_ITaskService, (void**)&svc))) {
        CoUninitialize(); return -1;
    }
    VARIANT v_empty;
    VariantInit(&v_empty);
    svc->Connect(v_empty, v_empty, v_empty, v_empty);
    ITaskFolder *folder = NULL;
    BSTR root = SysAllocString(TASK_FOLDER);
    HRESULT hr = svc->GetFolder(root, &folder);
    SysFreeString(root);
    if (FAILED(hr)) { svc->Release(); CoUninitialize(); return -1; }
    BSTR name_bstr = SysAllocString(TASK_NAME);
    hr = folder->DeleteTask(name_bstr, 0);
    SysFreeString(name_bstr);
    folder->Release();
    svc->Release();
    CoUninitialize();
    return SUCCEEDED(hr) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Command dispatch                                                   */
/* ------------------------------------------------------------------ */

static int handle_command(SOCKET s, char *line) {
    char *cmd = line;
    char *arg = strchr(line, ' ');
    if (arg) { *arg++ = 0; while (*arg == ' ') arg++; }
    else arg = (char*)"";

    /* ---- Connection ---- */
    if (!_stricmp(cmd, "PING"))    { send_str(s, "OK pong\n"); return 0; }
    if (!_stricmp(cmd, "QUIT") || !_stricmp(cmd, "EXIT") || !_stricmp(cmd, "BYE")) {
        send_ok(s); return 1;
    }
    if (!_stricmp(cmd, "CAPS")) {
        char buf[2048];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "OK PING CAPS INFO QUIT ABORT "
            "SCREEN MPOS SHOT SHOTRECT SHOTWIN WATCH WAITFOR "
            "READ WRITE LIST STAT DELETE MKDIR RENAME "
            "KEY KEYS KEYDOWN KEYUP "
            "MOVE MOVEREL CLICK DCLICK MDOWN MUP DRAG WHEEL "
            "EXEC RUN WAIT KILL PS SLEEP "
            "CLIPGET CLIPSET "
            "WINFIND WINLIST WINACTIVE WINMOVE WINSIZE WINFOCUS WINCLOSE WINMIN WINMAX WINRESTORE "
            "ENV IDLE DRIVES LOCK"
            "%s%s\n",
            uia_available() ? " ELEMENTS ELEMENT_AT ELEMENT_FIND ELEMENT_INVOKE ELEMENT_FOCUS"
                              " ELEMENT_TREE ELEMENT_TEXT ELEMENT_SET_TEXT"
                              " ELEMENT_TOGGLE ELEMENT_EXPAND ELEMENT_COLLAPSE" : "",
            g_enable_power ? " LOGOFF REBOOT SHUTDOWN" : "");
        send_str(s, buf);
        return 0;
    }
    if (!_stricmp(cmd, "ABORT")) {
        InterlockedIncrement(&g_abort_generation);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "INFO")) {
        char buf[1024];
        char user[128] = "?";
        char host[128] = "?";
        DWORD ulen = sizeof(user), hlen = sizeof(host);
        GetUserNameA(user, &ulen);
        GetComputerNameA(host, &hlen);
        /* WebP is statically linked unconditionally — always available.
           PNG depends on Gdiplus startup. capture=wgc when WGC initialised
           successfully (Win10 1803+); falls back to capture=gdi (BitBlt)
           on older / locked-down hosts. */
        const char *fmts = g_png_available ? "bmp,png,webp" : "bmp,webp";
        const char *capture = wgc_available() ? "wgc" : "gdi";
        const char *ui_auto = uia_available() ? "uia" : "no";
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "OK os=windows-modern arch=x64 protocol=1 "
            "capture=%s multi_monitor=yes dpi_aware=yes cursor_in_shot=yes "
            "input=sendinput "
            "path_encoding=utf8 max_path=32767 "
            "windows=yes "
            "user=%s hostname=%s "
            "max_connections=%d "
            "image_formats=%s "
            "ui_automation=%s "
            "discovery=%s "
            "auto_start=task power=%s\n",
            capture, user, host, MAX_CONNECTIONS,
            fmts, ui_auto,
            discovery_active() ? "mdns" : "no",
            g_enable_power ? "yes" : "no");
        send_str(s, buf);
        return 0;
    }

    /* ---- Screen ---- */
    if (!_stricmp(cmd, "SCREEN")) {
        int x, y, w, h;
        virtual_screen_rect(&x, &y, &w, &h);
        char rsp[64];
        _snprintf_s(rsp, sizeof(rsp), _TRUNCATE, "OK %d %d\n", w, h);
        send_str(s, rsp);
        return 0;
    }
    if (!_stricmp(cmd, "MPOS")) {
        /* GetCursorPos can succeed-with-stale-data when there's no
           interactive desktop session (e.g. agent running as a
           Vagrant-provisioned logon task before a real GUI login).
           Without explicitly checking, we'd return uninitialised stack.
           Initialise to a sentinel and validate the call. */
        POINT p = { -1, -1 };
        if (!GetCursorPos(&p)) { send_err(s, "no desktop session"); return 0; }
        /* Sanity-check against the virtual screen rect. Anything outside
           plausible bounds means no real session. */
        int vsx, vsy, vsw, vsh;
        virtual_screen_rect(&vsx, &vsy, &vsw, &vsh);
        if (p.x < vsx - 1 || p.x > vsx + vsw + 1 ||
            p.y < vsy - 1 || p.y > vsy + vsh + 1) {
            send_err(s, "no desktop session");
            return 0;
        }
        char rsp[64];
        _snprintf_s(rsp, sizeof(rsp), _TRUNCATE, "OK %ld %ld\n", p.x, p.y);
        send_str(s, rsp);
        return 0;
    }
    if (!_stricmp(cmd, "SHOT")) {
        CaptureFormat format = parse_image_format(&arg);
        if (format.kind == IMG_PNG && !g_png_available) { send_err(s, "format unsupported"); return 0; }
        BYTE *buf = NULL; DWORD sz = 0;
        int rc = capture_image(format, &buf, &sz);
        if (rc == -2) { send_err(s, "format unsupported"); return 0; }
        if (rc < 0)   { send_err(s, "capture failed"); return 0; }
        send_ok_data(s, buf, sz);
        free(buf);
        return 0;
    }
    if (!_stricmp(cmd, "SHOTRECT")) {
        CaptureFormat format = parse_image_format(&arg);
        if (format.kind == IMG_PNG && !g_png_available) { send_err(s, "format unsupported"); return 0; }
        int x, y, w, h;
        if (sscanf_s(arg, "%d %d %d %d", &x, &y, &w, &h) != 4) {
            send_err(s, "usage: SHOTRECT [bmp|png|webp[:NN]] x y w h"); return 0;
        }
        if (w <= 0 || h <= 0) { send_err(s, "invalid size"); return 0; }
        BYTE *buf = NULL; DWORD sz = 0;
        int rc = capture_image_rect(format, x, y, w, h, &buf, &sz);
        if (rc == -2) { send_err(s, "format unsupported"); return 0; }
        if (rc < 0)   { send_err(s, "capture failed"); return 0; }
        send_ok_data(s, buf, sz);
        free(buf);
        return 0;
    }
    if (!_stricmp(cmd, "SHOTWIN")) {
        CaptureFormat format = parse_image_format(&arg);
        if (format.kind == IMG_PNG && !g_png_available) { send_err(s, "format unsupported"); return 0; }
        if (!*arg) { send_err(s, "usage: SHOTWIN [bmp|png|webp[:NN]] title"); return 0; }
        HWND hwnd = find_window_by_title_prefix(arg);
        if (!hwnd) { send_err(s, "window not found"); return 0; }
        RECT r;
        if (!GetWindowRect(hwnd, &r)) { send_err(s, "GetWindowRect failed"); return 0; }
        /* WGC per-window path captures DirectComposition / acrylic / WinUI 3
           surfaces correctly; falls back to BitBlt of the window's screen
           rect on systems / windows where WGC can't attach. */
        HBITMAP hbm = capture_window_to_hbitmap(hwnd, r);
        if (!hbm) { send_err(s, "capture failed"); return 0; }
        BITMAP bm;
        GetObject(hbm, sizeof(bm), &bm);
        int w = bm.bmWidth, h = bm.bmHeight;
        BYTE *buf = NULL; DWORD sz = 0;
        int rc = hbitmap_to_format(hbm, w, h, format, &buf, &sz);
        DeleteObject(hbm);
        if (rc == -2) { send_err(s, "format unsupported"); return 0; }
        if (rc < 0)   { send_err(s, "encode failed"); return 0; }
        send_ok_data(s, buf, sz);
        free(buf);
        return 0;
    }
    if (!_stricmp(cmd, "WATCH")) {
        CaptureFormat format = parse_image_format(&arg);
        if (format.kind == IMG_PNG && !g_png_available) { send_err(s, "format unsupported"); return 0; }
        int interval_ms, duration_ms;
        double threshold_pct = 1.0;
        if (sscanf_s(arg, "%d %d %lf", &interval_ms, &duration_ms, &threshold_pct) < 2) {
            send_err(s, "usage: WATCH [bmp|png|webp[:NN]] interval_ms duration_ms [threshold_pct]");
            return 0;
        }
        /* Hold a single WGC session open across the WATCH for modern-shell
           coverage at frame-rate. Per-call WGC setup (~50 ms) would saturate
           the agent at any reasonable interval; the session amortises that
           cost across every frame. Guard struct closes the session on every
           exit path (normal end, abort, send failure). */
        wgc_session_guard frame_src;
        if (interval_ms < 50)        interval_ms = 50;
        if (duration_ms <= 0)        { send_err(s, "invalid duration"); return 0; }
        if (duration_ms > 600000)    duration_ms = 600000;
        if (threshold_pct < 0.0)     threshold_pct = 0.0;
        if (threshold_pct > 100.0)   threshold_pct = 100.0;

        BYTE thumb[THUMB_BYTES];
        BYTE prev_thumb[THUMB_BYTES];
        bool has_prev = false;
        DWORD start = GetTickCount();
        DWORD next_tick = start;
        LONG abort_baseline = g_abort_generation;

        while (true) {
            DWORD now = GetTickCount();
            DWORD elapsed = now - start;
            if (elapsed >= (DWORD)duration_ms) break;
            if (g_abort_generation != abort_baseline) {
                send_str(s, "ERR aborted\n");
                return 0;
            }
            if (capture_thumbnail(thumb) < 0) break;
            bool send_frame = !has_prev;
            if (has_prev && thumbnail_diff_pct(thumb, prev_thumb) > threshold_pct) {
                send_frame = true;
            }
            if (send_frame) {
                /* Streaming WGC frame (or BitBlt fallback) → format-encode
                   → write. Session amortises WGC setup cost across the
                   whole WATCH; one HBITMAP per emitted frame. */
                HBITMAP frame_bmp = frame_src.capture_frame();
                if (!frame_bmp) break;
                BITMAP bm; GetObject(frame_bmp, sizeof(bm), &bm);
                BYTE *bmp = NULL;
                DWORD bmp_size = 0;
                int rc = hbitmap_to_format(frame_bmp, bm.bmWidth, bm.bmHeight,
                                           format, &bmp, &bmp_size);
                DeleteObject(frame_bmp);
                if (rc < 0) break;
                char hdr[64];
                int hdr_n = _snprintf_s(hdr, sizeof(hdr), _TRUNCATE, "OK %lu %lu\n",
                                        (unsigned long)elapsed, (unsigned long)bmp_size);
                if (send_all(s, hdr, hdr_n) < 0) { free(bmp); return 0; }
                if (bmp_size > 0 && send_all(s, (const char*)bmp, (int)bmp_size) < 0) {
                    free(bmp); return 0;
                }
                free(bmp);
                memcpy(prev_thumb, thumb, sizeof(prev_thumb));
                has_prev = true;
            }
            next_tick += interval_ms;
            now = GetTickCount();
            if (next_tick > now) {
                DWORD remaining = next_tick - now;
                while (remaining > 0) {
                    DWORD step = remaining > 50 ? 50 : remaining;
                    if (g_abort_generation != abort_baseline) {
                        send_str(s, "ERR aborted\n");
                        return 0;
                    }
                    Sleep(step);
                    remaining -= step;
                }
            } else {
                next_tick = now;
            }
        }
        send_str(s, "END\n");
        return 0;
    }
    if (!_stricmp(cmd, "WAITFOR")) {
        CaptureFormat format = parse_image_format(&arg);
        if (format.kind == IMG_PNG && !g_png_available) { send_err(s, "format unsupported"); return 0; }
        int timeout_ms;
        double threshold_pct = 1.0;
        if (sscanf_s(arg, "%d %lf", &timeout_ms, &threshold_pct) < 1) {
            send_err(s, "usage: WAITFOR [bmp|png|webp[:NN]] timeout_ms [threshold_pct]");
            return 0;
        }
        /* WAITFOR may take many seconds polling — use the streaming session
           so the eventual returned frame captures modern-shell surfaces
           correctly without paying setup cost on every poll. */
        wgc_session_guard frame_src;
        if (timeout_ms <= 0)        { send_err(s, "invalid timeout"); return 0; }
        if (timeout_ms > 600000)    timeout_ms = 600000;
        if (threshold_pct < 0.0)    threshold_pct = 0.0;
        if (threshold_pct > 100.0)  threshold_pct = 100.0;

        BYTE baseline[THUMB_BYTES];
        BYTE current[THUMB_BYTES];
        if (capture_thumbnail(baseline) < 0) {
            send_err(s, "capture failed");
            return 0;
        }
        DWORD start = GetTickCount();
        LONG abort_baseline = g_abort_generation;
        while (true) {
            if (g_abort_generation != abort_baseline) {
                send_err(s, "aborted");
                return 0;
            }
            DWORD elapsed = GetTickCount() - start;
            if (elapsed >= (DWORD)timeout_ms) {
                send_err(s, "timeout");
                return 0;
            }
            Sleep(50);
            if (capture_thumbnail(current) < 0) continue;
            if (thumbnail_diff_pct(current, baseline) > threshold_pct) {
                HBITMAP frame_bmp = frame_src.capture_frame();
                if (!frame_bmp) { send_err(s, "capture failed"); return 0; }
                BITMAP bm; GetObject(frame_bmp, sizeof(bm), &bm);
                BYTE *bmp = NULL;
                DWORD bmp_size = 0;
                int rc = hbitmap_to_format(frame_bmp, bm.bmWidth, bm.bmHeight,
                                           format, &bmp, &bmp_size);
                DeleteObject(frame_bmp);
                if (rc < 0) { send_err(s, "encode failed"); return 0; }
                char hdr[64];
                int hdr_n = _snprintf_s(hdr, sizeof(hdr), _TRUNCATE, "OK %lu %lu\n",
                                        (unsigned long)elapsed, (unsigned long)bmp_size);
                send_all(s, hdr, hdr_n);
                if (bmp_size > 0) send_all(s, (const char*)bmp, (int)bmp_size);
                free(bmp);
                return 0;
            }
        }
    }

    /* ---- Files ---- */
    if (!_stricmp(cmd, "READ")) {
        BYTE *buf = NULL; DWORD sz = 0;
        if (!*arg) { send_err(s, "missing path"); return 0; }
        if (read_whole_file(arg, &buf, &sz) < 0) { send_err(s, "read failed"); return 0; }
        send_ok_data(s, buf, sz);
        free(buf);
        return 0;
    }
    if (!_stricmp(cmd, "WRITE")) {
        char *space = strrchr(arg, ' ');
        if (!space) { send_err(s, "usage: WRITE <path> <length>"); return 0; }
        DWORD len = (DWORD)atol(space + 1);
        if (len > MAX_WRITE_BYTES) { send_err(s, "payload too large"); return 0; }
        size_t pathlen = (size_t)(space - arg);
        if (pathlen >= MAX_PATH) { send_err(s, "path too long"); return 0; }
        char path[MAX_PATH];
        memcpy(path, arg, pathlen); path[pathlen] = 0;
        BYTE *buf = (BYTE*)malloc(len ? len : 1);
        if (!buf) { send_err(s, "oom"); return 0; }
        if (len > 0 && recv_n(s, buf, (int)len) < 0) { free(buf); send_err(s, "short read"); return 0; }
        if (write_whole_file(path, buf, len) < 0) { free(buf); send_err(s, "write failed"); return 0; }
        free(buf);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "LIST")) {
        if (!*arg) { send_err(s, "missing path"); return 0; }
        BYTE *buf = NULL; DWORD sz = 0;
        if (list_dir(arg, &buf, &sz) < 0) { send_err(s, "list failed"); return 0; }
        send_ok_data(s, buf, sz);
        free(buf);
        return 0;
    }
    if (!_stricmp(cmd, "STAT")) {
        if (!*arg) { send_err(s, "missing path"); return 0; }
        char type;
        unsigned __int64 size;
        long mtime;
        if (do_stat(arg, &type, &size, &mtime) < 0) { send_err(s, "not found"); return 0; }
        char rsp[128];
        _snprintf_s(rsp, sizeof(rsp), _TRUNCATE, "OK %c %I64u %ld\n", type, size, mtime);
        send_str(s, rsp);
        return 0;
    }
    if (!_stricmp(cmd, "DELETE")) {
        if (!*arg) { send_err(s, "missing path"); return 0; }
        if (do_delete(arg) < 0) { send_err(s, "delete failed"); return 0; }
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "MKDIR")) {
        if (!*arg) { send_err(s, "missing path"); return 0; }
        if (!CreateDirectoryA(arg, NULL)) { send_err(s, "mkdir failed"); return 0; }
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "RENAME")) {
        char *sep = strchr(arg, '\t');
        if (!sep) sep = strchr(arg, ' ');
        if (!sep) { send_err(s, "usage: RENAME src dst"); return 0; }
        *sep++ = 0;
        while (*sep == ' ' || *sep == '\t') sep++;
        if (!MoveFileA(arg, sep)) { send_err(s, "rename failed"); return 0; }
        send_ok(s);
        return 0;
    }

    /* ---- Keyboard ---- */
    if (!_stricmp(cmd, "KEY")) {
        if (!*arg) { send_err(s, "missing key"); return 0; }
        if (!press_key_combo(arg)) { send_err(s, "unknown key"); return 0; }
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "KEYS")) {
        type_string(arg);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "KEYDOWN")) {
        int vk = parse_vk(arg);
        if (vk < 0) { send_err(s, "unknown key"); return 0; }
        send_key((WORD)vk, 0);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "KEYUP")) {
        int vk = parse_vk(arg);
        if (vk < 0) { send_err(s, "unknown key"); return 0; }
        send_key((WORD)vk, KEYEVENTF_KEYUP);
        send_ok(s);
        return 0;
    }

    /* ---- Mouse ---- */
    if (!_stricmp(cmd, "MOVE")) {
        int x, y;
        if (sscanf_s(arg, "%d %d", &x, &y) != 2) { send_err(s, "usage: MOVE x y"); return 0; }
        SetCursorPos(x, y);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "MOVEREL")) {
        int dx, dy;
        if (sscanf_s(arg, "%d %d", &dx, &dy) != 2) { send_err(s, "usage: MOVEREL dx dy"); return 0; }
        POINT p;
        GetCursorPos(&p);
        SetCursorPos(p.x + dx, p.y + dy);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "CLICK"))  { do_click(parse_button(arg), 0); send_ok(s); return 0; }
    if (!_stricmp(cmd, "DCLICK")) { do_click(parse_button(arg), 1); send_ok(s); return 0; }
    if (!_stricmp(cmd, "MDOWN"))  { do_mdown(parse_button(arg)); send_ok(s); return 0; }
    if (!_stricmp(cmd, "MUP"))    { do_mup(parse_button(arg));   send_ok(s); return 0; }
    if (!_stricmp(cmd, "DRAG")) {
        int x, y, btn = 1;
        char btn_name[16] = "";
        int n = sscanf_s(arg, "%d %d %15s", &x, &y, btn_name, (unsigned)sizeof(btn_name));
        if (n < 2) { send_err(s, "usage: DRAG x y [button]"); return 0; }
        if (n >= 3) btn = parse_button(btn_name);
        do_mdown(btn); Sleep(20);
        SetCursorPos(x, y); Sleep(20);
        do_mup(btn);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "WHEEL")) { do_wheel(atoi(arg)); send_ok(s); return 0; }

    /* ---- Process ---- */
    if (!_stricmp(cmd, "EXEC")) {
        if (!*arg) { send_err(s, "missing cmdline"); return 0; }
        DWORD pid = 0;
        if (do_exec(arg, &pid) < 0) { send_err(s, "CreateProcess failed"); return 0; }
        char rsp[64];
        _snprintf_s(rsp, sizeof(rsp), _TRUNCATE, "OK %lu\n", (unsigned long)pid);
        send_str(s, rsp);
        return 0;
    }
    if (!_stricmp(cmd, "RUN")) {
        if (!*arg) { send_err(s, "missing cmdline"); return 0; }
        DWORD exit_code = 0;
        BYTE *buf = NULL; DWORD sz = 0;
        int rc = do_run(arg, &exit_code, &buf, &sz);
        if (rc == -3) { send_err(s, "aborted"); return 0; }
        if (rc < 0)   { send_err(s, "CreateProcess failed"); return 0; }
        char hdr[64];
        int n = _snprintf_s(hdr, sizeof(hdr), _TRUNCATE, "OK %lu %lu\n",
                            (unsigned long)exit_code, (unsigned long)sz);
        send_all(s, hdr, n);
        if (sz > 0) send_all(s, (const char*)buf, (int)sz);
        free(buf);
        return 0;
    }
    if (!_stricmp(cmd, "WAIT")) {
        DWORD pid = 0, timeout = INFINITE, exit_code = 0;
        if (sscanf_s(arg, "%lu %lu", &pid, &timeout) < 1) { send_err(s, "usage: WAIT pid [timeout_ms]"); return 0; }
        int rc = do_wait(pid, timeout, &exit_code);
        if (rc == -1) { send_err(s, "OpenProcess failed"); return 0; }
        if (rc == -2) { send_err(s, "timeout"); return 0; }
        if (rc == -3) { send_err(s, "aborted"); return 0; }
        char rsp[64];
        _snprintf_s(rsp, sizeof(rsp), _TRUNCATE, "OK %lu\n", (unsigned long)exit_code);
        send_str(s, rsp);
        return 0;
    }
    if (!_stricmp(cmd, "KILL")) {
        DWORD pid = (DWORD)atol(arg);
        if (pid == 0) { send_err(s, "invalid pid"); return 0; }
        if (do_kill(pid) < 0) { send_err(s, "kill failed"); return 0; }
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "PS")) {
        BYTE *buf = NULL; DWORD sz = 0;
        if (list_processes(&buf, &sz) < 0) { send_err(s, "ps failed"); return 0; }
        send_ok_data(s, buf, sz);
        free(buf);
        return 0;
    }
    if (!_stricmp(cmd, "SLEEP")) {
        DWORD remaining = (DWORD)atol(arg);
        LONG abort_baseline = g_abort_generation;
        while (remaining > 0) {
            DWORD step = remaining > 100 ? 100 : remaining;
            if (g_abort_generation != abort_baseline) {
                send_err(s, "aborted");
                return 0;
            }
            Sleep(step);
            remaining -= step;
        }
        send_ok(s);
        return 0;
    }

    /* ---- Clipboard ---- */
    if (!_stricmp(cmd, "CLIPGET")) {
        BYTE *buf = NULL; DWORD sz = 0;
        if (clip_get(&buf, &sz) < 0) { send_err(s, "clipboard read failed"); return 0; }
        send_ok_data(s, buf, sz);
        free(buf);
        return 0;
    }
    if (!_stricmp(cmd, "CLIPSET")) {
        if (!*arg) { send_err(s, "usage: CLIPSET <length>"); return 0; }
        DWORD len = (DWORD)atol(arg);
        if (len > MAX_CLIPSET_BYTES) { send_err(s, "payload too large"); return 0; }
        BYTE *buf = (BYTE*)malloc(len ? len : 1);
        if (!buf) { send_err(s, "oom"); return 0; }
        if (len > 0 && recv_n(s, buf, (int)len) < 0) { free(buf); send_err(s, "short read"); return 0; }
        if (clip_set(buf, len) < 0) { free(buf); send_err(s, "clipboard write failed"); return 0; }
        free(buf);
        send_ok(s);
        return 0;
    }

    /* ---- Windows ---- */
    if (!_stricmp(cmd, "WINLIST")) {
        BYTE *buf = NULL; DWORD sz = 0;
        if (list_windows(&buf, &sz) < 0) { send_err(s, "list failed"); return 0; }
        send_ok_data(s, buf, sz);
        free(buf);
        return 0;
    }
    if (!_stricmp(cmd, "WINFIND")) {
        char title[128] = "";
        if (sscanf_s(arg, "%127[^\r\n]", title, (unsigned)sizeof(title)) < 1) {
            send_err(s, "usage: WINFIND title"); return 0;
        }
        HWND hwnd = find_window_by_title_prefix(title);
        if (!hwnd) { send_err(s, "window not found"); return 0; }
        RECT r;
        GetWindowRect(hwnd, &r);
        char rsp[64];
        _snprintf_s(rsp, sizeof(rsp), _TRUNCATE, "OK %ld %ld %ld %ld\n",
                    r.left, r.top, r.right - r.left, r.bottom - r.top);
        send_str(s, rsp);
        return 0;
    }
    if (!_stricmp(cmd, "WINACTIVE")) {
        HWND hwnd = GetForegroundWindow();
        if (!hwnd) { send_err(s, "no active window"); return 0; }
        char title[256] = "";
        GetWindowTextA(hwnd, title, sizeof(title));
        RECT r;
        GetWindowRect(hwnd, &r);
        char rsp[512];
        _snprintf_s(rsp, sizeof(rsp), _TRUNCATE,
                    "OK %llu\t%ld\t%ld\t%ld\t%ld\t%s\n",
                    (unsigned long long)(ULONG_PTR)hwnd,
                    (long)r.left, (long)r.top,
                    (long)(r.right - r.left), (long)(r.bottom - r.top),
                    title);
        send_str(s, rsp);
        return 0;
    }
    if (!_stricmp(cmd, "WINMOVE")) {
        int x, y;
        char title[128] = "";
        if (sscanf_s(arg, "%d %d %127[^\r\n]", &x, &y, title, (unsigned)sizeof(title)) < 3) {
            send_err(s, "usage: WINMOVE x y title"); return 0;
        }
        HWND hwnd = find_window_by_title_prefix(title);
        if (!hwnd) { send_err(s, "window not found"); return 0; }
        if (!SetWindowPos(hwnd, HWND_TOP, x, y, 0, 0,
                          SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW)) {
            send_err(s, "SetWindowPos failed"); return 0;
        }
        SetForegroundWindow(hwnd);
        RECT r;
        GetWindowRect(hwnd, &r);
        char rsp[64];
        _snprintf_s(rsp, sizeof(rsp), _TRUNCATE, "OK %ld %ld %ld %ld\n",
                    r.left, r.top, r.right - r.left, r.bottom - r.top);
        send_str(s, rsp);
        return 0;
    }
    if (!_stricmp(cmd, "WINSIZE")) {
        int w, h;
        char title[128] = "";
        if (sscanf_s(arg, "%d %d %127[^\r\n]", &w, &h, title, (unsigned)sizeof(title)) < 3) {
            send_err(s, "usage: WINSIZE w h title"); return 0;
        }
        HWND hwnd = find_window_by_title_prefix(title);
        if (!hwnd) { send_err(s, "window not found"); return 0; }
        if (!SetWindowPos(hwnd, HWND_TOP, 0, 0, w, h,
                          SWP_NOMOVE | SWP_NOZORDER | SWP_SHOWWINDOW)) {
            send_err(s, "SetWindowPos failed"); return 0;
        }
        RECT r;
        GetWindowRect(hwnd, &r);
        char rsp[64];
        _snprintf_s(rsp, sizeof(rsp), _TRUNCATE, "OK %ld %ld %ld %ld\n",
                    r.left, r.top, r.right - r.left, r.bottom - r.top);
        send_str(s, rsp);
        return 0;
    }
    if (!_stricmp(cmd, "WINFOCUS") || !_stricmp(cmd, "WINCLOSE") ||
        !_stricmp(cmd, "WINMIN") || !_stricmp(cmd, "WINMAX") ||
        !_stricmp(cmd, "WINRESTORE")) {
        if (!*arg) { send_err(s, "usage: <verb> title"); return 0; }
        HWND hwnd = find_window_by_title_prefix(arg);
        if (!hwnd) { send_err(s, "window not found"); return 0; }
        if (!_stricmp(cmd, "WINFOCUS")) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        } else if (!_stricmp(cmd, "WINCLOSE")) PostMessageA(hwnd, WM_CLOSE, 0, 0);
        else if (!_stricmp(cmd, "WINMIN"))     ShowWindow(hwnd, SW_MINIMIZE);
        else if (!_stricmp(cmd, "WINMAX"))     ShowWindow(hwnd, SW_MAXIMIZE);
        else                                   ShowWindow(hwnd, SW_RESTORE);
        send_ok(s);
        return 0;
    }

    /* ---- UI Automation ---- */
    if (!_stricmp(cmd, "ELEMENTS")) {
        if (!uia_available()) { send_err(s, "ui_automation unavailable"); return 0; }
        int rect[4];
        bool has_rect = false;
        if (*arg) {
            if (sscanf_s(arg, "%d %d %d %d", &rect[0], &rect[1], &rect[2], &rect[3]) == 4) {
                has_rect = true;
            } else {
                send_err(s, "usage: ELEMENTS [x y w h]");
                return 0;
            }
        }
        BYTE *buf = NULL; DWORD sz = 0;
        if (uia_elements(has_rect ? rect : NULL, &buf, &sz) < 0) {
            send_err(s, "elements failed"); return 0;
        }
        send_ok_data(s, buf, sz);
        free(buf);
        return 0;
    }
    if (!_stricmp(cmd, "ELEMENT_AT")) {
        if (!uia_available()) { send_err(s, "ui_automation unavailable"); return 0; }
        int x, y;
        if (sscanf_s(arg, "%d %d", &x, &y) != 2) {
            send_err(s, "usage: ELEMENT_AT x y"); return 0;
        }
        char *row = NULL;
        if (uia_element_at(x, y, &row) < 0) { send_err(s, "not found"); return 0; }
        char rsp[2048];
        _snprintf_s(rsp, sizeof(rsp), _TRUNCATE, "OK %s\n", row);
        send_str(s, rsp);
        free(row);
        return 0;
    }
    if (!_stricmp(cmd, "ELEMENT_FIND")) {
        if (!uia_available()) { send_err(s, "ui_automation unavailable"); return 0; }
        char role[32] = "";
        char name[256] = "";
        /* role first (whitespace-delimited), then optional name substring
           that runs to end of line. */
        int n = sscanf_s(arg, "%31s %255[^\r\n]",
                         role, (unsigned)sizeof(role),
                         name, (unsigned)sizeof(name));
        if (n < 1 || !role[0]) {
            send_err(s, "usage: ELEMENT_FIND role [name-substring]");
            return 0;
        }
        char *row = NULL;
        if (uia_element_find(role, name, &row) < 0) {
            send_err(s, "not found"); return 0;
        }
        char rsp[2048];
        _snprintf_s(rsp, sizeof(rsp), _TRUNCATE, "OK %s\n", row);
        send_str(s, rsp);
        free(row);
        return 0;
    }
    if (!_stricmp(cmd, "ELEMENT_INVOKE")) {
        if (!uia_available()) { send_err(s, "ui_automation unavailable"); return 0; }
        int id = atoi(arg);
        if (id <= 0) { send_err(s, "usage: ELEMENT_INVOKE id"); return 0; }
        int rc = uia_element_invoke(id);
        if (rc == -2) { send_err(s, "id"); return 0; }
        if (rc == -3) { send_err(s, "not invokable"); return 0; }
        if (rc < 0)   { send_err(s, "invoke failed"); return 0; }
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "ELEMENT_FOCUS")) {
        if (!uia_available()) { send_err(s, "ui_automation unavailable"); return 0; }
        int id = atoi(arg);
        if (id <= 0) { send_err(s, "usage: ELEMENT_FOCUS id"); return 0; }
        int rc = uia_element_focus(id);
        if (rc == -2) { send_err(s, "id"); return 0; }
        if (rc < 0)   { send_err(s, "focus failed"); return 0; }
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "ELEMENT_TREE")) {
        if (!uia_available()) { send_err(s, "ui_automation unavailable"); return 0; }
        int id = atoi(arg);
        if (id <= 0) { send_err(s, "usage: ELEMENT_TREE id"); return 0; }
        BYTE *buf = NULL; DWORD sz = 0;
        int rc = uia_element_tree(id, &buf, &sz);
        if (rc == -2) { send_err(s, "id"); return 0; }
        if (rc < 0)   { send_err(s, "tree failed"); return 0; }
        send_ok_data(s, buf, sz);
        free(buf);
        return 0;
    }
    if (!_stricmp(cmd, "ELEMENT_TEXT")) {
        if (!uia_available()) { send_err(s, "ui_automation unavailable"); return 0; }
        int id = atoi(arg);
        if (id <= 0) { send_err(s, "usage: ELEMENT_TEXT id"); return 0; }
        BYTE *buf = NULL; DWORD sz = 0;
        int rc = uia_element_text(id, &buf, &sz);
        if (rc == -2) { send_err(s, "id"); return 0; }
        if (rc < 0)   { send_err(s, "text failed"); return 0; }
        send_ok_data(s, buf, sz);
        free(buf);
        return 0;
    }
    if (!_stricmp(cmd, "ELEMENT_SET_TEXT")) {
        if (!uia_available()) { send_err(s, "ui_automation unavailable"); return 0; }
        /* Format: ELEMENT_SET_TEXT <id> <length>\n<bytes> — same shape
           as WRITE / CLIPSET so existing length-prefixed-payload framing
           applies. */
        char *space = strchr(arg, ' ');
        if (!space) { send_err(s, "usage: ELEMENT_SET_TEXT <id> <length>"); return 0; }
        int id = atoi(arg);
        DWORD len = (DWORD)atol(space + 1);
        if (id <= 0) { send_err(s, "usage: ELEMENT_SET_TEXT <id> <length>"); return 0; }
        if (len > MAX_CLIPSET_BYTES) { send_err(s, "payload too large"); return 0; }
        BYTE *payload = (BYTE*)malloc(len ? len : 1);
        if (!payload) { send_err(s, "oom"); return 0; }
        if (len > 0 && recv_n(s, payload, (int)len) < 0) {
            free(payload); send_err(s, "short read"); return 0;
        }
        int rc = uia_element_set_text(id, payload, len);
        free(payload);
        if (rc == -2) { send_err(s, "id"); return 0; }
        if (rc == -3) { send_err(s, "no value pattern"); return 0; }
        if (rc < 0)   { send_err(s, "set failed"); return 0; }
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "ELEMENT_TOGGLE")) {
        if (!uia_available()) { send_err(s, "ui_automation unavailable"); return 0; }
        int id = atoi(arg);
        if (id <= 0) { send_err(s, "usage: ELEMENT_TOGGLE id"); return 0; }
        int rc = uia_element_toggle(id);
        if (rc == -2) { send_err(s, "id"); return 0; }
        if (rc == -3) { send_err(s, "not toggleable"); return 0; }
        if (rc < 0)   { send_err(s, "toggle failed"); return 0; }
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "ELEMENT_EXPAND") || !_stricmp(cmd, "ELEMENT_COLLAPSE")) {
        if (!uia_available()) { send_err(s, "ui_automation unavailable"); return 0; }
        int id = atoi(arg);
        if (id <= 0) { send_err(s, "usage: <verb> id"); return 0; }
        int rc = !_stricmp(cmd, "ELEMENT_EXPAND") ? uia_element_expand(id)
                                                   : uia_element_collapse(id);
        if (rc == -2) { send_err(s, "id"); return 0; }
        if (rc == -3) { send_err(s, "not expandable"); return 0; }
        if (rc < 0)   { send_err(s, "expand/collapse failed"); return 0; }
        send_ok(s);
        return 0;
    }

    /* ---- System ---- */
    if (!_stricmp(cmd, "ENV")) {
        if (*arg) {
            char val[4096];
            DWORD n = GetEnvironmentVariableA(arg, val, sizeof(val));
            if (n == 0) { send_err(s, "not set"); return 0; }
            if (n >= sizeof(val)) { send_err(s, "value too long"); return 0; }
            char rsp[4200];
            _snprintf_s(rsp, sizeof(rsp), _TRUNCATE, "OK %s\n", val);
            send_str(s, rsp);
        } else {
            BYTE *buf = NULL; DWORD sz = 0;
            if (list_env(&buf, &sz) < 0) { send_err(s, "env failed"); return 0; }
            send_ok_data(s, buf, sz);
            free(buf);
        }
        return 0;
    }
    if (!_stricmp(cmd, "IDLE")) {
        DWORD secs = 0;
        if (get_idle_seconds(&secs) < 0) { send_err(s, "idle failed"); return 0; }
        char rsp[32];
        _snprintf_s(rsp, sizeof(rsp), _TRUNCATE, "OK %lu\n", (unsigned long)secs);
        send_str(s, rsp);
        return 0;
    }
    if (!_stricmp(cmd, "DRIVES")) {
        BYTE *buf = NULL; DWORD sz = 0;
        if (list_drives(&buf, &sz) < 0) { send_err(s, "drives failed"); return 0; }
        send_ok_data(s, buf, sz);
        free(buf);
        return 0;
    }
    if (!_stricmp(cmd, "LOCK")) {
        if (do_lock() < 0) { send_err(s, "lock failed"); return 0; }
        send_ok(s);
        return 0;
    }

    /* ---- Power (gated) ---- */
    if (!_stricmp(cmd, "LOGOFF") || !_stricmp(cmd, "REBOOT") || !_stricmp(cmd, "SHUTDOWN")) {
        if (!g_enable_power) { send_err(s, "power verbs disabled"); return 0; }
        UINT flags;
        if (!_stricmp(cmd, "LOGOFF"))      flags = EWX_LOGOFF;
        else if (!_stricmp(cmd, "REBOOT")) flags = EWX_REBOOT;
        else                               flags = EWX_POWEROFF;
        if (flags != EWX_LOGOFF) enable_shutdown_privilege();
        send_ok(s);
        ExitWindowsEx(flags, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER);
        return 1;
    }

    send_err(s, "unknown command");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Server loop                                                        */
/* ------------------------------------------------------------------ */

/* Per-thread worker. Mirrors windows-nt/agent.c — see that file for the
   rationale on the SEH crash barrier and the last-connection cleanup. */
struct conn_args {
    SOCKET sock;
    char addr[64];
};

static unsigned __stdcall connection_worker(void *arg) {
    conn_args *ca = (conn_args*)arg;
    SOCKET c = ca->sock;
    char addr[64];
    strncpy_s(addr, sizeof(addr), ca->addr, _TRUNCATE);
    free(ca);

    /* WinRT / WGC requires a COM-initialised apartment per thread. No-op
       when WGC isn't available. Paired with wgc_thread_uninit() at exit.
       UIA's per-thread element map gets its own init alongside. */
    wgc_thread_init();
    uia_thread_init();

    printf("[+] %s connected\n", addr); fflush(stdout);

    while (1) {
        char line[LINEBUF];
        int n = recv_line(c, line, sizeof(line));
        int close_conn = 0;
        DWORD exc_code = 0;
        if (n < 0) break;
        if (n == 0) continue;
        printf("[%s] > %s\n", addr, line); fflush(stdout);
        __try {
            if (handle_command(c, line) == 1) close_conn = 1;
        } __except (exc_code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
            fprintf(stderr, "[%s] exception 0x%08lx in handler for '%s'\n",
                    addr, (unsigned long)exc_code, line);
            fflush(stderr);
            send_str(c, "ERR internal\n");
            close_conn = 1;
        }
        if (close_conn) break;
    }
    closesocket(c);

    EnterCriticalSection(&g_conn_lock);
    g_active_connections--;
    bool last = (g_active_connections == 0);
    LeaveCriticalSection(&g_conn_lock);
    if (last) cleanup_input_state();

    uia_thread_uninit();
    wgc_thread_uninit();

    printf("[-] %s disconnected\n", addr); fflush(stdout);
    return 0;
}

static void serve(SOCKET listener) {
    while (1) {
        struct sockaddr_in cli;
        int cli_len = sizeof(cli);
        SOCKET c = accept(listener, (struct sockaddr*)&cli, &cli_len);
        if (c == INVALID_SOCKET) continue;

        EnterCriticalSection(&g_conn_lock);
        bool admit = (g_active_connections < MAX_CONNECTIONS);
        if (admit) g_active_connections++;
        LeaveCriticalSection(&g_conn_lock);
        if (!admit) {
            send_str(c, "ERR busy\n");
            closesocket(c);
            continue;
        }

        conn_args *ca = (conn_args*)malloc(sizeof(*ca));
        if (!ca) {
            EnterCriticalSection(&g_conn_lock);
            g_active_connections--;
            LeaveCriticalSection(&g_conn_lock);
            send_str(c, "ERR oom\n");
            closesocket(c);
            continue;
        }
        ca->sock = c;
        inet_ntop(AF_INET, &cli.sin_addr, ca->addr, sizeof(ca->addr));

        uintptr_t h = _beginthreadex(NULL, 0, connection_worker, ca, 0, NULL);
        if (h == 0) {
            EnterCriticalSection(&g_conn_lock);
            g_active_connections--;
            LeaveCriticalSection(&g_conn_lock);
            send_str(c, "ERR thread\n");
            closesocket(c);
            free(ca);
            continue;
        }
        CloseHandle((HANDLE)h);
    }
}

static void init_gdiplus() {
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&g_gdiplus_token, &input, NULL) == Gdiplus::Ok) {
        g_png_available = true;
    }
}

static void cleanup_gdiplus() {
    if (g_png_available) {
        Gdiplus::GdiplusShutdown(g_gdiplus_token);
        g_png_available = false;
    }
}

static int run_server(void) {
    InitializeCriticalSection(&g_proc_lock);
    InitializeCriticalSection(&g_conn_lock);
    init_gdiplus();
    wgc_init();  /* Best-effort; falls back to BitBlt path if unavailable. */
    uia_init();  /* Best-effort; ELEMENT_* verbs ERR if unavailable. */

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fprintf(stderr, "WSAStartup failed\n"); return 1; }
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == INVALID_SOCKET) { fprintf(stderr, "socket failed\n"); return 1; }
    int opt = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    struct sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)g_port);
    if (bind(ls, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "bind failed (port %d in use?)\n", g_port);
        return 1;
    }
    listen(ls, MAX_CONNECTIONS);
    printf("agent-remote-hands (windows-modern) listening on 0.0.0.0:%d "
           "(power=%s, max_connections=%d, capture=%s)\n",
           g_port, g_enable_power ? "on" : "off", MAX_CONNECTIONS,
           wgc_available() ? "wgc" : "gdi");
    if (g_enable_discovery) {
        discovery_start((unsigned short)g_port, "windows-modern");
    }
    fflush(stdout);
    serve(ls);
    discovery_stop();
    closesocket(ls);
    WSACleanup();
    uia_shutdown();
    wgc_shutdown();
    cleanup_gdiplus();
    DeleteCriticalSection(&g_proc_lock);
    DeleteCriticalSection(&g_conn_lock);
    return 0;
}

static void print_usage(void) {
    printf("agent-remote-hands (windows-modern)\n"
           "\n"
           "Usage:\n"
           "  remote-hands.exe [<port>]      run in foreground (default port 8765)\n"
           "  remote-hands.exe --install     register Task Scheduler 'at-logon' task\n"
           "  remote-hands.exe --uninstall   remove the task\n"
           "  remote-hands.exe --discoverable advertise on the LAN via mDNS\n"
           "\n"
           "Env:\n"
           "  REMOTE_HANDS_PORT          override listen port\n"
           "  REMOTE_HANDS_POWER=1       enable LOGOFF/REBOOT/SHUTDOWN verbs\n"
           "  REMOTE_HANDS_DISCOVERABLE=1 advertise via mDNS (no auth — only on trusted LANs)\n");
}

int main(int argc, char **argv) {
    /* DPI awareness — manifest sets it, but call the API too as belt-and-braces. */
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef BOOL (WINAPI *PSetProcessDpiAwarenessContext)(HANDLE);
        auto pSet = (PSetProcessDpiAwarenessContext)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (pSet) pSet((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */);
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    char *env_port      = getenv("REMOTE_HANDS_PORT");
    char *env_power     = getenv("REMOTE_HANDS_POWER");
    char *env_discover  = getenv("REMOTE_HANDS_DISCOVERABLE");
    if (!env_port) env_port = getenv("VM_AGENT_PORT");
    if (env_port) g_port = atoi(env_port);
    if (env_power && atoi(env_power) != 0) g_enable_power = 1;
    if (env_discover && atoi(env_discover) != 0) g_enable_discovery = 1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--install") || !_stricmp(argv[i], "/install")) {
            if (do_install() < 0) {
                fprintf(stderr, "install failed: %lu (run elevated?)\n", GetLastError());
                return 1;
            }
            printf("Task '%ws' registered. Will run at next logon.\n", TASK_NAME);
            return 0;
        }
        if (!strcmp(argv[i], "--uninstall") || !_stricmp(argv[i], "/uninstall")) {
            if (do_uninstall() < 0) {
                fprintf(stderr, "uninstall failed: %lu (run elevated?)\n", GetLastError());
                return 1;
            }
            printf("Task '%ws' removed.\n", TASK_NAME);
            return 0;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h") || !strcmp(argv[i], "/?")) {
            print_usage();
            return 0;
        }
        if (!strcmp(argv[i], "--discoverable")) {
            g_enable_discovery = 1;
            continue;
        }
        if (argv[i][0] >= '0' && argv[i][0] <= '9') {
            g_port = atoi(argv[i]);
            continue;
        }
        fprintf(stderr, "unknown argument: %s\n", argv[i]);
        print_usage();
        return 1;
    }

    return run_server();
}

/* agent.c — Agent Remote Hands, windows-nt target
 *
 * Minimal control agent for the NT-kernel pre-Vista line: NT4 / 2000 / XP /
 * Server 2003. Builds with VC6 + Win32 SDK; runs equivalently on each of
 * those Windows releases (the implementation sticks to APIs they all share).
 *
 * Listens on TCP 8765 (or VM_AGENT_PORT env, kept for back-compat). Text
 * protocol, one command per line, response always begins "OK" or "ERR".
 * Commands that return data use "OK <length>\n<bytes>". See PROTOCOL.md at
 * the repo root for the full spec.
 *
 * Build: cl /MT /O2 agent.c /link wsock32.lib user32.lib gdi32.lib advapi32.lib
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock.h>
#include <windows.h>
#include <winsvc.h>      /* WIN32_LEAN_AND_MEAN excludes this — needed for service mode. */
#include <tlhelp32.h>
#include <ole2.h>        /* IStream — used by GDI+ PNG encoder. */
#include <process.h>     /* _beginthreadex */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#pragma comment(lib, "wsock32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")  /* CreateStreamOnHGlobal / GetHGlobalFromStream */

/* Defensive fallbacks for VC6 stock SDK headers that pre-date Windows 2000. */
#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#endif
#ifndef FILE_ATTRIBUTE_REPARSE_POINT
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400
#endif

#define DEFAULT_PORT 8765
#define LINEBUF 8192

/* Size caps. The agent is single-threaded and a single rogue request can
   exhaust the heap; cap every unbounded payload so a bad client gets an
   ERR rather than crashing the binary. */
#define MAX_WRITE_BYTES    (256 * 1024 * 1024)  /* WRITE payload */
#define MAX_CLIPSET_BYTES   (16 * 1024 * 1024)  /* CLIPSET payload */
#define MAX_RUN_OUTPUT      (16 * 1024 * 1024)  /* RUN captured stdout/stderr */
#define MAX_LIST_BYTES      ( 8 * 1024 * 1024)  /* LIST rows */
#define MAX_PS_BYTES        ( 4 * 1024 * 1024)  /* PS rows */
#define MAX_WINLIST_BYTES   ( 4 * 1024 * 1024)  /* WINLIST rows */
#define MAX_ENV_BYTES       ( 1 * 1024 * 1024)  /* ENV rows */

#define SERVICE_NAME       "RemoteHands"
#define SERVICE_DISPLAY    "Agent Remote Hands"

/* Concurrency: each accepted client runs on its own worker thread so a
   long-running verb (WATCH, RUN, big READ) on one connection doesn't block
   commands on another. Cap to keep resource use bounded; further connects
   get an immediate ERR busy. */
#define MAX_CONNECTIONS 4

static int g_port = DEFAULT_PORT;
static int g_enable_power = 0;  /* opt-in via REMOTE_HANDS_POWER=1 */
static SERVICE_STATUS_HANDLE g_svc_handle = NULL;
static SERVICE_STATUS g_svc_status;

/* Mutex protecting the EXEC/WAIT process table below. Shared mutable state
   is the only thing that needs locking — Win32 file/clipboard/GDI calls
   are safe from multiple threads given each thread owns its own handles. */
static CRITICAL_SECTION g_proc_lock;
/* Tracks live connection count so we can refuse over MAX_CONNECTIONS and
   detect the "last connection disconnecting" moment for input cleanup. */
static CRITICAL_SECTION g_conn_lock;
static int g_active_connections = 0;

/* Global generation counter for the ABORT verb. Each long-running verb
   (RUN, SLEEP, WAIT with timeout, WATCH, WAITFOR) snapshots this at start
   and checks each loop iteration. ABORT bumps it via InterlockedIncrement,
   which causes every in-flight long verb across every connection to break
   out and return ERR aborted. Lock-free; cheap; per-process. */
static volatile LONG g_abort_generation = 0;

/* GDI+ for PNG encoding. We load gdiplus.dll dynamically rather than
   linking statically so the binary still runs on hosts without GDI+ (NT4
   pre-redist install); on those, image_formats=bmp only. The flat C API
   exports are resolved at startup via GetProcAddress — same pattern as
   GetCursorInfo / LockWorkStation elsewhere in this file. */
typedef struct {
    UINT32 GdiplusVersion;
    void *DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} RH_GdiplusStartupInput;
typedef int (WINAPI *RH_PGdiplusStartup)(ULONG_PTR *token, const RH_GdiplusStartupInput *input, void *output);
typedef VOID (WINAPI *RH_PGdiplusShutdown)(ULONG_PTR token);
typedef int (WINAPI *RH_PGdipCreateBitmapFromHBITMAP)(HBITMAP hbm, HPALETTE pal, void **out_bmp);
typedef int (WINAPI *RH_PGdipDisposeImage)(void *image);
typedef int (WINAPI *RH_PGdipSaveImageToStream)(void *image, IStream *stream, const CLSID *clsid_encoder, const void *params);
static HMODULE g_gdiplus_dll = NULL;
static ULONG_PTR g_gdiplus_token = 0;
static int g_png_available = 0;
static RH_PGdiplusStartup pGdiplusStartup = NULL;
static RH_PGdiplusShutdown pGdiplusShutdown = NULL;
static RH_PGdipCreateBitmapFromHBITMAP pGdipCreateBitmapFromHBITMAP = NULL;
static RH_PGdipDisposeImage pGdipDisposeImage = NULL;
static RH_PGdipSaveImageToStream pGdipSaveImageToStream = NULL;

static void init_gdiplus(void) {
    RH_GdiplusStartupInput input;
    g_gdiplus_dll = LoadLibraryA("gdiplus.dll");
    if (!g_gdiplus_dll) return;
    pGdiplusStartup = (RH_PGdiplusStartup)GetProcAddress(g_gdiplus_dll, "GdiplusStartup");
    pGdiplusShutdown = (RH_PGdiplusShutdown)GetProcAddress(g_gdiplus_dll, "GdiplusShutdown");
    pGdipCreateBitmapFromHBITMAP = (RH_PGdipCreateBitmapFromHBITMAP)GetProcAddress(g_gdiplus_dll, "GdipCreateBitmapFromHBITMAP");
    pGdipDisposeImage = (RH_PGdipDisposeImage)GetProcAddress(g_gdiplus_dll, "GdipDisposeImage");
    pGdipSaveImageToStream = (RH_PGdipSaveImageToStream)GetProcAddress(g_gdiplus_dll, "GdipSaveImageToStream");
    if (!pGdiplusStartup || !pGdiplusShutdown || !pGdipCreateBitmapFromHBITMAP ||
        !pGdipDisposeImage || !pGdipSaveImageToStream) return;
    input.GdiplusVersion = 1;
    input.DebugEventCallback = NULL;
    input.SuppressBackgroundThread = FALSE;
    input.SuppressExternalCodecs = FALSE;
    if (pGdiplusStartup(&g_gdiplus_token, &input, NULL) != 0) return;
    g_png_available = 1;
}

static void cleanup_gdiplus(void) {
    if (g_png_available && pGdiplusShutdown) {
        pGdiplusShutdown(g_gdiplus_token);
        g_png_available = 0;
    }
    if (g_gdiplus_dll) {
        FreeLibrary(g_gdiplus_dll);
        g_gdiplus_dll = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Network helpers                                                    */
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
    _snprintf(buf, sizeof(buf), "ERR %s\n", msg);
    return send_str(s, buf);
}

static int send_ok_data(SOCKET s, const BYTE *data, DWORD size) {
    char hdr[64];
    int n = _snprintf(hdr, sizeof(hdr), "OK %lu\n", (unsigned long)size);
    if (send_all(s, hdr, n) < 0) return -1;
    if (size > 0) return send_all(s, (const char*)data, (int)size);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Key name --> VK mapping                                            */
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

static void press_key_combo(const char *spec) {
    int mods[8];
    int n_mods = 0;
    char base[64];
    const char *p = spec;
    int i, base_vk;
    while (1) {
        const char *sep = strpbrk(p, "-+");
        char tok[16];
        size_t len;
        int vk;
        if (!sep) break;
        len = sep - p;
        if (len >= sizeof(tok)) break;
        memcpy(tok, p, len);
        tok[len] = 0;
        vk = parse_vk(tok);
        if (vk < 0 || n_mods >= 7) break;
        mods[n_mods++] = vk;
        p = sep + 1;
    }
    strncpy(base, p, sizeof(base) - 1);
    base[sizeof(base) - 1] = 0;
    base_vk = parse_vk(base);
    if (base_vk < 0) return;
    for (i = 0; i < n_mods; i++) keybd_event((BYTE)mods[i], 0, 0, 0);
    keybd_event((BYTE)base_vk, 0, 0, 0);
    Sleep(20);
    keybd_event((BYTE)base_vk, 0, KEYEVENTF_KEYUP, 0);
    for (i = n_mods - 1; i >= 0; i--) keybd_event((BYTE)mods[i], 0, KEYEVENTF_KEYUP, 0);
}

static void type_string(const char *str) {
    while (*str) {
        SHORT scan = VkKeyScanA(*str);
        BYTE vk, shift;
        if (scan == -1) { str++; continue; }
        vk = (BYTE)(scan & 0xFF);
        shift = (BYTE)((scan >> 8) & 0xFF);
        if (shift & 1) keybd_event(VK_SHIFT, 0, 0, 0);
        if (shift & 2) keybd_event(VK_CONTROL, 0, 0, 0);
        if (shift & 4) keybd_event(VK_MENU, 0, 0, 0);
        keybd_event(vk, 0, 0, 0);
        keybd_event(vk, 0, KEYEVENTF_KEYUP, 0);
        if (shift & 4) keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        if (shift & 2) keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
        if (shift & 1) keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
        Sleep(5);
        str++;
    }
}

/* ------------------------------------------------------------------ */
/* Mouse                                                              */
/* ------------------------------------------------------------------ */

static void button_flags(int button, DWORD *down, DWORD *up) {
    if (button == 2) { *down = MOUSEEVENTF_RIGHTDOWN;  *up = MOUSEEVENTF_RIGHTUP; }
    else if (button == 3) { *down = MOUSEEVENTF_MIDDLEDOWN; *up = MOUSEEVENTF_MIDDLEUP; }
    else { *down = MOUSEEVENTF_LEFTDOWN; *up = MOUSEEVENTF_LEFTUP; }
}

static void do_click(int button, int double_click) {
    DWORD down, up;
    button_flags(button, &down, &up);
    mouse_event(down, 0, 0, 0, 0);
    Sleep(20);
    mouse_event(up, 0, 0, 0, 0);
    if (double_click) {
        Sleep(50);
        mouse_event(down, 0, 0, 0, 0);
        Sleep(20);
        mouse_event(up, 0, 0, 0, 0);
    }
}

static void do_mdown(int button) {
    DWORD down, up;
    button_flags(button, &down, &up);
    mouse_event(down, 0, 0, 0, 0);
}

static void do_mup(int button) {
    DWORD down, up;
    button_flags(button, &down, &up);
    mouse_event(up, 0, 0, 0, 0);
}

static void do_wheel(int delta) {
    /* WHEEL_DELTA is 120 per notch; pass user value directly. */
    mouse_event(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)delta, 0);
}

static int parse_button(const char *name) {
    if (!name || !*name) return 1;
    if (!_stricmp(name, "left") || !_stricmp(name, "1")) return 1;
    if (!_stricmp(name, "right") || !_stricmp(name, "2")) return 2;
    if (!_stricmp(name, "middle") || !_stricmp(name, "3")) return 3;
    return 1;
}

/* Best-effort release of any held mouse buttons / modifier keys left over
   from a previous client. KEYUP/MUP for an unheld key is a no-op, so this
   is safe to call between connections. */
static void cleanup_input_state(void) {
    keybd_event(VK_SHIFT,   0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_MENU,    0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_LWIN,    0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_RWIN,    0, KEYEVENTF_KEYUP, 0);
    mouse_event(MOUSEEVENTF_LEFTUP,   0, 0, 0, 0);
    mouse_event(MOUSEEVENTF_RIGHTUP,  0, 0, 0, 0);
    mouse_event(MOUSEEVENTF_MIDDLEUP, 0, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Screenshot                                                          */
/* ------------------------------------------------------------------ */

/* Image-format token: bmp (default) or png. Negotiated via the leading
   token of the capture verb's args; see parse_image_format(). */
#define IMG_BMP 0
#define IMG_PNG 1

/* Returns the captured screen rect as a 24bpp HBITMAP with the OS cursor
   composited in. Caller must DeleteObject. The cursor compositing is the
   reason this is shared between BMP and PNG paths — BitBlt strips the
   hardware cursor and the controller needs to see it for click positioning. */
static HBITMAP capture_screen_to_hbitmap(int sx, int sy, int w, int h) {
    HDC hdc_screen = GetDC(NULL);
    HDC hdc_mem;
    HBITMAP hbm;
    HGDIOBJ old_obj;
    if (!hdc_screen) return NULL;
    hdc_mem = CreateCompatibleDC(hdc_screen);
    hbm = CreateCompatibleBitmap(hdc_screen, w, h);
    old_obj = SelectObject(hdc_mem, hbm);
    BitBlt(hdc_mem, 0, 0, w, h, hdc_screen, sx, sy, SRCCOPY);

    {
        /* CURSORINFO post-dates the VC6 SDK so it's declared inline;
           GetCursorInfo lives in user32.dll, available on XP+. */
        typedef struct tagCURSORINFO_ {
            DWORD cbSize;
            DWORD flags;
            HCURSOR hCursor;
            POINT ptScreenPos;
        } CURSORINFO_;
        typedef BOOL (WINAPI *PGetCursorInfo)(CURSORINFO_*);
        static PGetCursorInfo pGetCursorInfo = NULL;
        static int resolved = 0;
        if (!resolved) {
            HMODULE hUser = GetModuleHandle("user32.dll");
            if (hUser) pGetCursorInfo = (PGetCursorInfo)GetProcAddress(hUser, "GetCursorInfo");
            resolved = 1;
        }
        if (pGetCursorInfo) {
            CURSORINFO_ ci;
            ci.cbSize = sizeof(ci);
            if (pGetCursorInfo(&ci) && (ci.flags & 0x00000001 /*CURSOR_SHOWING*/) && ci.hCursor) {
                ICONINFO ii;
                int hx = 0, hy = 0;
                if (GetIconInfo(ci.hCursor, &ii)) {
                    hx = (int)ii.xHotspot; hy = (int)ii.yHotspot;
                    if (ii.hbmMask)  DeleteObject(ii.hbmMask);
                    if (ii.hbmColor) DeleteObject(ii.hbmColor);
                }
                DrawIcon(hdc_mem, ci.ptScreenPos.x - hx - sx, ci.ptScreenPos.y - hy - sy, ci.hCursor);
            }
        }
    }

    SelectObject(hdc_mem, old_obj);
    DeleteDC(hdc_mem);
    ReleaseDC(NULL, hdc_screen);
    return hbm;
}

/* Serializes a 24bpp HBITMAP to a Windows BMP file in memory. */
static int hbitmap_to_bmp(HBITMAP hbm, int w, int h, BYTE **out_buf, DWORD *out_size) {
    HDC hdc;
    BITMAPINFO bi;
    int row;
    DWORD px_size, total;
    BYTE *buf;
    BITMAPFILEHEADER *bfh;

    hdc = GetDC(NULL);
    if (!hdc) return -1;

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  /* negative = top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    row = ((w * 3 + 3) & ~3);
    px_size = (DWORD)row * h;
    total = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + px_size;

    buf = (BYTE*)malloc(total);
    if (!buf) { ReleaseDC(NULL, hdc); return -1; }
    bfh = (BITMAPFILEHEADER*)buf;
    bfh->bfType = 0x4D42;  /* 'BM' */
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

/* Encodes an HBITMAP to a PNG via GDI+. The PNG encoder CLSID is stable
   across all GDI+ versions; hardcoding it skips the encoder enumeration. */
static int hbitmap_to_png(HBITMAP hbm, BYTE **out_buf, DWORD *out_size) {
    static const CLSID PNG_CLSID =
        { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
    void *gp_image = NULL;
    IStream *stream = NULL;
    HGLOBAL hg = NULL;
    SIZE_T size;
    BYTE *src;
    BYTE *out;
    int status;

    if (!g_png_available) return -2;
    if (pGdipCreateBitmapFromHBITMAP(hbm, NULL, &gp_image) != 0 || !gp_image) return -1;
    if (CreateStreamOnHGlobal(NULL, TRUE, &stream) != S_OK || !stream) {
        pGdipDisposeImage(gp_image);
        return -1;
    }
    status = pGdipSaveImageToStream(gp_image, stream, &PNG_CLSID, NULL);
    pGdipDisposeImage(gp_image);
    if (status != 0) {
        stream->lpVtbl->Release(stream);
        return -1;
    }
    GetHGlobalFromStream(stream, &hg);
    size = GlobalSize(hg);
    src = (BYTE*)GlobalLock(hg);
    out = (BYTE*)malloc(size ? size : 1);
    if (!out) {
        GlobalUnlock(hg);
        stream->lpVtbl->Release(stream);
        return -1;
    }
    if (size) memcpy(out, src, size);
    GlobalUnlock(hg);
    stream->lpVtbl->Release(stream);

    *out_buf = out;
    *out_size = (DWORD)size;
    return 0;
}

/* Format-aware capture. format ∈ {IMG_BMP, IMG_PNG}. Returns 0 on success,
   -1 on capture/encode failure, -2 if format=PNG and GDI+ unavailable. */
static int capture_image_rect(int format, int sx, int sy, int w, int h,
                              BYTE **out_buf, DWORD *out_size) {
    HBITMAP hbm = capture_screen_to_hbitmap(sx, sy, w, h);
    int rc;
    if (!hbm) return -1;
    if (format == IMG_PNG) {
        rc = hbitmap_to_png(hbm, out_buf, out_size);
    } else {
        rc = hbitmap_to_bmp(hbm, w, h, out_buf, out_size);
    }
    DeleteObject(hbm);
    return rc;
}

static int capture_image(int format, BYTE **out_buf, DWORD *out_size) {
    return capture_image_rect(format, 0, 0,
                              GetSystemMetrics(SM_CXSCREEN),
                              GetSystemMetrics(SM_CYSCREEN),
                              out_buf, out_size);
}

/* BMP-only convenience wrappers, kept for callers that don't care about
   format (WATCH/WAITFOR previously, now also format-aware). */
static int capture_bmp_rect(int sx, int sy, int w, int h, BYTE **out_buf, DWORD *out_size) {
    return capture_image_rect(IMG_BMP, sx, sy, w, h, out_buf, out_size);
}

static int capture_bmp(BYTE **out_buf, DWORD *out_size) {
    return capture_image(IMG_BMP, out_buf, out_size);
}

/* Parses an optional leading "bmp" or "png" token from arg, advancing arg
   past it (and any following spaces). Returns IMG_BMP or IMG_PNG. Absent
   token defaults to IMG_BMP for back-compat. */
static int parse_image_format(char **arg_p) {
    char *arg = *arg_p;
    int fmt = IMG_BMP;
    int consume = 0;
    if (_strnicmp(arg, "bmp", 3) == 0 && (arg[3] == 0 || arg[3] == ' ')) {
        fmt = IMG_BMP; consume = 3;
    } else if (_strnicmp(arg, "png", 3) == 0 && (arg[3] == 0 || arg[3] == ' ')) {
        fmt = IMG_PNG; consume = 3;
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

/* Captures the screen as a 32x32 24-bit RGB (BGR ordering — Windows DIB)
   thumbnail. Cursor is NOT composited (the screen DC's BitBlt source omits
   the hardware cursor by default), so cursor movement alone never changes
   the thumbnail and won't trigger a WATCH frame. Output buffer is
   THUMB_BYTES = 3072 bytes (row stride 32*3 = 96 is already 4-aligned, so
   no padding). */
static int capture_thumbnail(BYTE *out_thumb) {
    HDC hdc_screen = GetDC(NULL);
    HDC hdc_mem;
    HBITMAP hbm;
    HGDIOBJ old_obj;
    BITMAPINFO bi;
    int sw, sh;
    if (!hdc_screen) return -1;
    sw = GetSystemMetrics(SM_CXSCREEN);
    sh = GetSystemMetrics(SM_CYSCREEN);
    hdc_mem = CreateCompatibleDC(hdc_screen);
    hbm = CreateCompatibleBitmap(hdc_screen, THUMB_SIZE, THUMB_SIZE);
    old_obj = SelectObject(hdc_mem, hbm);
    SetStretchBltMode(hdc_mem, HALFTONE);
    SetBrushOrgEx(hdc_mem, 0, 0, NULL);
    StretchBlt(hdc_mem, 0, 0, THUMB_SIZE, THUMB_SIZE,
               hdc_screen, 0, 0, sw, sh, SRCCOPY);
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = THUMB_SIZE;
    bi.bmiHeader.biHeight = -THUMB_SIZE;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    GetDIBits(hdc_mem, hbm, 0, THUMB_SIZE, out_thumb, &bi, DIB_RGB_COLORS);
    SelectObject(hdc_mem, old_obj);
    DeleteObject(hbm);
    DeleteDC(hdc_mem);
    ReleaseDC(NULL, hdc_screen);
    return 0;
}

/* Returns the percentage (0.0 .. 100.0) of thumbnail pixels where any
   colour channel differs by more than 8/255 between A and B. Used by
   WATCH / WAITFOR to filter idle frames. */
static double thumbnail_diff_pct(const BYTE *a, const BYTE *b) {
    int changed = 0;
    int i;
    for (i = 0; i < THUMB_SIZE * THUMB_SIZE; i++) {
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
/* File I/O                                                           */
/* ------------------------------------------------------------------ */

static int read_whole_file(const char *path, BYTE **out_buf, DWORD *out_size) {
    HANDLE h;
    DWORD sz, got = 0;
    BYTE *buf;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    sz = GetFileSize(h, NULL);
    buf = (BYTE*)malloc(sz ? sz : 1);
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
    HANDLE h;
    DWORD wrote = 0;
    BOOL ok;
    h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    ok = WriteFile(h, buf, size, &wrote, NULL);
    CloseHandle(h);
    return (ok && wrote == size) ? 0 : -1;
}

static long filetime_to_unix(const FILETIME *ft) {
    /* FILETIME is 100ns ticks since 1601-01-01; Unix epoch starts 1970-01-01.
       Diff is 11644473600 seconds. Cast through signed __int64 so files dated
       before 1970 produce a negative result rather than wrapping. */
    ULARGE_INTEGER u;
    u.LowPart = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    return (long)((__int64)(u.QuadPart / 10000000ULL) - 11644473600LL);
}

static int list_dir(const char *path, BYTE **out_buf, DWORD *out_size) {
    char pattern[MAX_PATH];
    HANDLE h;
    WIN32_FIND_DATAA fd;
    char *buf;
    size_t cap = 4096, len = 0;
    int n;
    n = _snprintf(pattern, sizeof(pattern), "%s\\*", path);
    if (n < 0 || n >= (int)sizeof(pattern)) return -1;
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    buf = (char*)malloc(cap);
    if (!buf) { FindClose(h); return -1; }
    do {
        char line[MAX_PATH + 64];
        char type;
        ULARGE_INTEGER sz;
        long mtime;
        int line_len;
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) type = 'D';
        else if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) type = 'L';
        else type = 'F';
        sz.LowPart = fd.nFileSizeLow;
        sz.HighPart = fd.nFileSizeHigh;
        mtime = filetime_to_unix(&fd.ftLastWriteTime);
        line_len = _snprintf(line, sizeof(line), "%c\t%I64u\t%ld\t%s\n",
                             type, sz.QuadPart, mtime, fd.cFileName);
        if (line_len < 0) continue;
        if (len + (size_t)line_len > MAX_LIST_BYTES) break;  /* truncate rather than OOM */
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
    ULARGE_INTEGER u;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return -1;
    if (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) *type_out = 'D';
    else if (fa.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) *type_out = 'L';
    else *type_out = 'F';
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
    strncpy(buf, cmdline, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
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
    int slot = -1, i;
    LONG abort_baseline = g_abort_generation;
    DWORD start, total_waited = 0;
    EnterCriticalSection(&g_proc_lock);
    for (i = 0; i < g_proc_count; i++) {
        if (g_proc_handles[i] && g_proc_pids[i] == pid) {
            h = g_proc_handles[i]; slot = i; break;
        }
    }
    LeaveCriticalSection(&g_proc_lock);
    if (!h) {
        h = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (!h) return -1;
    }
    /* Wait outside the lock, in 250 ms chunks so ABORT can interrupt and so
       INFINITE waits aren't actually infinite for cancellation purposes. */
    start = GetTickCount();
    while (1) {
        DWORD chunk = 250;
        DWORD wr;
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
        wr = WaitForSingleObject(h, chunk);
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
    BOOL ok;
    if (!h) return -1;
    ok = TerminateProcess(h, 1);
    CloseHandle(h);
    return ok ? 0 : -1;
}

static int list_processes(BYTE **out_buf, DWORD *out_size) {
    HANDLE snap;
    PROCESSENTRY32 pe;
    char *buf;
    size_t cap = 8192, len = 0;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    pe.dwSize = sizeof(pe);
    if (!Process32First(snap, &pe)) { CloseHandle(snap); return -1; }
    buf = (char*)malloc(cap);
    if (!buf) { CloseHandle(snap); return -1; }
    do {
        char line[MAX_PATH + 32];
        int n = _snprintf(line, sizeof(line), "%lu\t%s\n",
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

/* Append `got` bytes of `chunk` to the growing output buffer, capped at
   MAX_RUN_OUTPUT. Returns 0 on success, -1 on OOM. */
static int run_append(char **buf_p, size_t *len_p, size_t *cap_p,
                      const char *chunk, DWORD got) {
    size_t len = *len_p, cap = *cap_p;
    DWORD write = got;
    if (len >= MAX_RUN_OUTPUT) return 0;  /* silently drop further bytes */
    if (len + write > MAX_RUN_OUTPUT) write = (DWORD)(MAX_RUN_OUTPUT - len);
    if (len + write > cap) {
        char *nb;
        while (len + write > cap) cap *= 2;
        nb = (char*)realloc(*buf_p, cap);
        if (!nb) return -1;
        *buf_p = nb; *cap_p = cap;
    }
    memcpy(*buf_p + len, chunk, write);
    *len_p = len + write;
    return 0;
}

/* Returns 0 on normal exit, -1 on CreateProcess/system failure, -3 on ABORT. */
static int do_run(const char *cmdline, DWORD *out_exit, BYTE **out_buf, DWORD *out_size) {
    /* Abort-aware: instead of blocking on ReadFile, poll the pipe via
       PeekNamedPipe and the process via WaitForSingleObject(50). Each
       iteration checks g_abort_generation; on bump, TerminateProcess and
       return -3. The cost is ~50 ms latency on completion vs. blocking
       reads — negligible for most use cases. */
    SECURITY_ATTRIBUTES sa;
    HANDLE rd, wr, hNul;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char buf[1024];
    char *output;
    size_t cap = 4096, len = 0;
    char chunk[4096];
    LONG abort_baseline = g_abort_generation;
    int aborted = 0;

    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    hNul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, &sa,
                      OPEN_EXISTING, 0, NULL);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = hNul;

    strncpy(buf, cmdline, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    if (!CreateProcessA(NULL, buf, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
        return -1;
    }
    CloseHandle(wr);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);

    output = (char*)malloc(cap);
    if (!output) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(rd); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        return -1;
    }

    while (1) {
        DWORD avail = 0;
        DWORD wr_status;

        /* Abort check */
        if (g_abort_generation != abort_baseline) { aborted = 1; break; }

        /* Drain any data currently in the pipe (non-blocking via Peek) */
        if (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            DWORD got = 0;
            DWORD to_read = avail > sizeof(chunk) ? sizeof(chunk) : avail;
            if (ReadFile(rd, chunk, to_read, &got, NULL) && got > 0) {
                if (run_append(&output, &len, &cap, chunk, got) < 0) {
                    /* OOM — abandon */
                    TerminateProcess(pi.hProcess, 1);
                    free(output);
                    CloseHandle(rd); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
                    return -1;
                }
            }
            continue;  /* keep draining before sleeping */
        }

        /* No pending data. Wait briefly for either: child exits, pipe gets
           data, or abort fires. WaitForSingleObject with a small timeout is
           the simplest compatible primitive. */
        wr_status = WaitForSingleObject(pi.hProcess, 50);
        if (wr_status == WAIT_OBJECT_0) {
            /* Child exited; drain any final bytes still in the pipe. */
            while (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                DWORD got = 0;
                DWORD to_read = avail > sizeof(chunk) ? sizeof(chunk) : avail;
                if (!ReadFile(rd, chunk, to_read, &got, NULL) || got == 0) break;
                if (run_append(&output, &len, &cap, chunk, got) < 0) break;
            }
            break;
        }
        /* WAIT_TIMEOUT: loop, check abort, drain again. */
    }

    if (aborted) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);  /* let TerminateProcess finalise */
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
    HANDLE h;
    char *p;
    char *buf;
    DWORD len;
    if (!OpenClipboard(NULL)) return -1;
    h = GetClipboardData(CF_TEXT);
    if (!h) {
        CloseClipboard();
        buf = (char*)malloc(1);
        if (!buf) return -1;
        *out_buf = (BYTE*)buf;
        *out_size = 0;
        return 0;
    }
    p = (char*)GlobalLock(h);
    if (!p) { CloseClipboard(); return -1; }
    len = (DWORD)strlen(p);
    buf = (char*)malloc(len ? len : 1);
    if (!buf) { GlobalUnlock(h); CloseClipboard(); return -1; }
    if (len) memcpy(buf, p, len);
    GlobalUnlock(h);
    CloseClipboard();
    *out_buf = (BYTE*)buf;
    *out_size = len;
    return 0;
}

static int clip_set(const BYTE *buf, DWORD len) {
    HGLOBAL h;
    char *p;
    if (!OpenClipboard(NULL)) return -1;
    EmptyClipboard();
    h = GlobalAlloc(GMEM_MOVEABLE, len + 1);
    if (!h) { CloseClipboard(); return -1; }
    p = (char*)GlobalLock(h);
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

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    int oom;
} winlist_ctx;

static BOOL CALLBACK enum_window_cb(HWND h, LPARAM lp) {
    winlist_ctx *ctx = (winlist_ctx*)lp;
    char title[256];
    RECT r;
    char line[512];
    int n;
    if (!IsWindowVisible(h)) return TRUE;
    if (GetWindowTextA(h, title, sizeof(title)) <= 0) return TRUE;
    if (!GetWindowRect(h, &r)) return TRUE;
    n = _snprintf(line, sizeof(line),
                  "%lu\t%ld\t%ld\t%ld\t%ld\t%s\n",
                  (unsigned long)(ULONG_PTR)h,
                  (long)r.left, (long)r.top,
                  (long)(r.right - r.left), (long)(r.bottom - r.top),
                  title);
    if (n < 0) return TRUE;
    if (ctx->len + (size_t)n > MAX_WINLIST_BYTES) return FALSE;  /* stop enumeration */
    if (ctx->len + (size_t)n > ctx->cap) {
        char *nb;
        size_t newcap = ctx->cap;
        while (ctx->len + (size_t)n > newcap) newcap *= 2;
        nb = (char*)realloc(ctx->buf, newcap);
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
    ctx.cap = 4096;
    ctx.len = 0;
    ctx.oom = 0;
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
    /* GetLastInputInfo is Windows 2000+; resolve dynamically so the binary
       still loads on plain XP installs that haven't shipped the symbol in
       a stripped user32.dll. (Belt-and-braces — XP always has it.) */
    typedef struct tagLASTINPUTINFO_ {
        UINT cbSize;
        DWORD dwTime;
    } LASTINPUTINFO_;
    typedef BOOL (WINAPI *PGetLastInputInfo)(LASTINPUTINFO_*);
    static PGetLastInputInfo pGetLastInputInfo = NULL;
    static int resolved = 0;
    LASTINPUTINFO_ lii;
    DWORD now;
    if (!resolved) {
        HMODULE h = GetModuleHandle("user32.dll");
        if (h) pGetLastInputInfo = (PGetLastInputInfo)GetProcAddress(h, "GetLastInputInfo");
        resolved = 1;
    }
    if (!pGetLastInputInfo) return -1;
    lii.cbSize = sizeof(lii);
    if (!pGetLastInputInfo(&lii)) return -1;
    now = GetTickCount();
    *out = (now - lii.dwTime) / 1000;
    return 0;
}

static int do_lock(void) {
    /* LockWorkStation is XP+; resolve dynamically to match the GetCursorInfo
       pattern above and keep the binary loadable on older builds. */
    typedef BOOL (WINAPI *PLockWorkStation)(VOID);
    static PLockWorkStation pLockWorkStation = NULL;
    static int resolved = 0;
    if (!resolved) {
        HMODULE h = GetModuleHandle("user32.dll");
        if (h) pLockWorkStation = (PLockWorkStation)GetProcAddress(h, "LockWorkStation");
        resolved = 1;
    }
    if (!pLockWorkStation) return -1;
    return pLockWorkStation() ? 0 : -1;
}

static int list_drives(BYTE **out_buf, DWORD *out_size) {
    char drives[512];
    DWORD got = GetLogicalDriveStringsA(sizeof(drives), drives);
    char *buf;
    size_t cap = 1024, len = 0;
    char *p;
    if (got == 0 || got > sizeof(drives)) return -1;
    buf = (char*)malloc(cap);
    if (!buf) return -1;
    for (p = drives; *p; p += strlen(p) + 1) {
        UINT t = GetDriveTypeA(p);
        const char *type;
        char line[64];
        int n;
        switch (t) {
            case DRIVE_REMOVABLE: type = "removable"; break;
            case DRIVE_FIXED:     type = "fixed";     break;
            case DRIVE_REMOTE:    type = "remote";    break;
            case DRIVE_CDROM:     type = "cdrom";     break;
            case DRIVE_RAMDISK:   type = "ramdisk";   break;
            default:              type = "unknown";   break;
        }
        n = _snprintf(line, sizeof(line), "%s\t%s\n", p, type);
        if (n < 0) continue;
        if (len + (size_t)n > cap) {
            char *nb;
            while (len + (size_t)n > cap) cap *= 2;
            nb = (char*)realloc(buf, cap);
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
    LPCH p;
    char *buf;
    size_t cap = 8192, len = 0;
    if (!env) return -1;
    buf = (char*)malloc(cap);
    if (!buf) { FreeEnvironmentStringsA(env); return -1; }
    for (p = env; *p; p += strlen(p) + 1) {
        size_t n;
        /* Skip cmd.exe's hidden "=C:=..." per-drive current-dir entries. */
        if (*p == '=') continue;
        n = strlen(p);
        if (len + n + 1 > MAX_ENV_BYTES) break;
        if (len + n + 1 > cap) {
            char *nb;
            while (len + n + 1 > cap) cap *= 2;
            nb = (char*)realloc(buf, cap);
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
    TOKEN_PRIVILEGES tp;
    LUID luid;
    BOOL ok;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return -1;
    if (!LookupPrivilegeValueA(NULL, SE_SHUTDOWN_NAME, &luid)) {
        CloseHandle(token); return -1;
    }
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL);
    CloseHandle(token);
    return (ok && GetLastError() == ERROR_SUCCESS) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Command dispatch                                                   */
/* ------------------------------------------------------------------ */

/* Returns 0 to continue, 1 to close client connection. */
static int handle_command(SOCKET s, char *line) {
    char *cmd = line;
    char *arg = strchr(line, ' ');
    if (arg) { *arg++ = 0; while (*arg == ' ') arg++; }
    else arg = "";

    /* ---- Connection control ---- */
    if (!_stricmp(cmd, "PING")) {
        send_str(s, "OK pong\n");
        return 0;
    }
    if (!_stricmp(cmd, "QUIT") || !_stricmp(cmd, "EXIT") || !_stricmp(cmd, "BYE")) {
        send_ok(s);
        return 1;
    }
    if (!_stricmp(cmd, "CAPS")) {
        char buf[2048];
        _snprintf(buf, sizeof(buf),
            "OK PING CAPS INFO QUIT ABORT "
            "SCREEN MPOS SHOT SHOTRECT SHOTWIN WATCH WAITFOR "
            "READ WRITE LIST STAT DELETE MKDIR RENAME "
            "KEY KEYS KEYDOWN KEYUP "
            "MOVE MOVEREL CLICK DCLICK MDOWN MUP DRAG WHEEL "
            "EXEC RUN WAIT KILL PS SLEEP "
            "CLIPGET CLIPSET "
            "WINFIND WINLIST WINACTIVE WINMOVE WINSIZE WINFOCUS WINCLOSE WINMIN WINMAX WINRESTORE "
            "ENV IDLE DRIVES LOCK"
            "%s\n",
            g_enable_power ? " LOGOFF REBOOT SHUTDOWN" : "");
        send_str(s, buf);
        return 0;
    }
    if (!_stricmp(cmd, "ABORT")) {
        /* Bump the global generation. Every long-running verb on every
           connection notices on its next loop iteration and returns
           ERR aborted. The connection that sent ABORT continues normally. */
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
        _snprintf(buf, sizeof(buf),
            "OK os=windows-nt arch=x86 protocol=1 "
            "capture=gdi multi_monitor=no dpi_aware=no cursor_in_shot=yes "
            "input=legacy "
            "path_encoding=ansi max_path=260 "
            "windows=yes "
            "user=%s hostname=%s "
            "max_connections=%d "
            "image_formats=%s "
            "power=%s\n",
            user, host, MAX_CONNECTIONS,
            g_png_available ? "bmp,png" : "bmp",
            g_enable_power ? "yes" : "no");
        send_str(s, buf);
        return 0;
    }

    /* ---- Screen / cursor ---- */
    if (!_stricmp(cmd, "SCREEN")) {
        char rsp[64];
        _snprintf(rsp, sizeof(rsp), "OK %d %d\n",
                  GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        send_str(s, rsp);
        return 0;
    }
    if (!_stricmp(cmd, "MPOS")) {
        POINT p;
        char rsp[64];
        GetCursorPos(&p);
        _snprintf(rsp, sizeof(rsp), "OK %ld %ld\n", p.x, p.y);
        send_str(s, rsp);
        return 0;
    }
    if (!_stricmp(cmd, "SHOT")) {
        int format = parse_image_format(&arg);
        BYTE *buf = NULL; DWORD sz = 0;
        int rc = capture_image(format, &buf, &sz);
        if (rc == -2) { send_err(s, "format unsupported"); return 0; }
        if (rc < 0)   { send_err(s, "capture failed"); return 0; }
        send_ok_data(s, buf, sz);
        free(buf);
        return 0;
    }
    if (!_stricmp(cmd, "SHOTRECT")) {
        int x, y, w, h;
        int format = parse_image_format(&arg);
        BYTE *buf = NULL; DWORD sz = 0;
        int rc;
        if (sscanf(arg, "%d %d %d %d", &x, &y, &w, &h) != 4) {
            send_err(s, "usage: SHOTRECT [bmp|png] x y w h"); return 0;
        }
        if (w <= 0 || h <= 0) { send_err(s, "invalid size"); return 0; }
        rc = capture_image_rect(format, x, y, w, h, &buf, &sz);
        if (rc == -2) { send_err(s, "format unsupported"); return 0; }
        if (rc < 0)   { send_err(s, "capture failed"); return 0; }
        send_ok_data(s, buf, sz);
        free(buf);
        return 0;
    }
    if (!_stricmp(cmd, "SHOTWIN")) {
        HWND hwnd;
        RECT r;
        int format = parse_image_format(&arg);
        BYTE *buf = NULL; DWORD sz = 0;
        int rc;
        if (!*arg) { send_err(s, "usage: SHOTWIN [bmp|png] title"); return 0; }
        hwnd = find_window_by_title_prefix(arg);
        if (!hwnd) { send_err(s, "window not found"); return 0; }
        if (!GetWindowRect(hwnd, &r)) { send_err(s, "GetWindowRect failed"); return 0; }
        rc = capture_image_rect(format, r.left, r.top,
                                r.right - r.left, r.bottom - r.top,
                                &buf, &sz);
        if (rc == -2) { send_err(s, "format unsupported"); return 0; }
        if (rc < 0)   { send_err(s, "capture failed"); return 0; }
        send_ok_data(s, buf, sz);
        free(buf);
        return 0;
    }
    if (!_stricmp(cmd, "WATCH")) {
        /* Stream of OK <ts_ms> <length>\n<frame> entries terminated by END\n.
           Frames are BMP or PNG depending on the leading format token (BMP
           default for back-compat). Sends the baseline frame, then only
           frames that differ from the previous by more than threshold_pct.
           Aborts on g_abort_generation bump or socket failure. */
        int interval_ms, duration_ms;
        double threshold_pct = 1.0;
        int format = parse_image_format(&arg);
        BYTE thumb[THUMB_BYTES];
        BYTE prev_thumb[THUMB_BYTES];
        int has_prev = 0;
        DWORD start, next_tick;
        LONG abort_baseline = g_abort_generation;
        if (format == IMG_PNG && !g_png_available) { send_err(s, "format unsupported"); return 0; }
        if (sscanf(arg, "%d %d %lf", &interval_ms, &duration_ms, &threshold_pct) < 2) {
            send_err(s, "usage: WATCH [bmp|png] interval_ms duration_ms [threshold_pct]");
            return 0;
        }
        if (interval_ms < 50)        interval_ms = 50;       /* sanity floor */
        if (duration_ms <= 0)        { send_err(s, "invalid duration"); return 0; }
        if (duration_ms > 600000)    duration_ms = 600000;   /* 10 min cap */
        if (threshold_pct < 0.0)     threshold_pct = 0.0;
        if (threshold_pct > 100.0)   threshold_pct = 100.0;

        start = GetTickCount();
        next_tick = start;
        while (1) {
            DWORD now = GetTickCount();
            DWORD elapsed = now - start;
            int send_frame;
            BYTE *bmp = NULL;
            DWORD bmp_size = 0;
            char hdr[64];
            int hdr_n;

            if (elapsed >= (DWORD)duration_ms) break;
            if (g_abort_generation != abort_baseline) {
                send_str(s, "ERR aborted\n");
                return 0;
            }

            if (capture_thumbnail(thumb) < 0) break;

            send_frame = !has_prev;  /* baseline always */
            if (has_prev) {
                if (thumbnail_diff_pct(thumb, prev_thumb) > threshold_pct) send_frame = 1;
            }
            if (send_frame) {
                if (capture_image(format, &bmp, &bmp_size) < 0) break;
                hdr_n = _snprintf(hdr, sizeof(hdr), "OK %lu %lu\n",
                                  (unsigned long)elapsed, (unsigned long)bmp_size);
                if (send_all(s, hdr, hdr_n) < 0) { free(bmp); return 0; }
                if (bmp_size > 0 && send_all(s, (const char*)bmp, (int)bmp_size) < 0) {
                    free(bmp); return 0;
                }
                free(bmp);
                memcpy(prev_thumb, thumb, sizeof(prev_thumb));
                has_prev = 1;
            }

            /* Sleep until the next interval boundary. If we fell behind,
               reset rather than chasing. */
            next_tick += interval_ms;
            now = GetTickCount();
            if (next_tick > now) {
                /* Chunk the sleep so abort takes effect within ~50 ms. */
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
                next_tick = now;  /* fell behind */
            }
        }
        send_str(s, "END\n");
        return 0;
    }
    if (!_stricmp(cmd, "WAITFOR")) {
        /* Polls (~50 ms) until the screen changes by more than threshold_pct
           vs. the baseline captured at the start, returns one frame, ends.
           Aborts on g_abort_generation bump. */
        int timeout_ms;
        double threshold_pct = 1.0;
        int format = parse_image_format(&arg);
        BYTE baseline[THUMB_BYTES];
        BYTE current[THUMB_BYTES];
        DWORD start;
        LONG abort_baseline = g_abort_generation;
        if (format == IMG_PNG && !g_png_available) { send_err(s, "format unsupported"); return 0; }
        if (sscanf(arg, "%d %lf", &timeout_ms, &threshold_pct) < 1) {
            send_err(s, "usage: WAITFOR [bmp|png] timeout_ms [threshold_pct]");
            return 0;
        }
        if (timeout_ms <= 0)        { send_err(s, "invalid timeout"); return 0; }
        if (timeout_ms > 600000)    timeout_ms = 600000;
        if (threshold_pct < 0.0)    threshold_pct = 0.0;
        if (threshold_pct > 100.0)  threshold_pct = 100.0;

        if (capture_thumbnail(baseline) < 0) {
            send_err(s, "capture failed");
            return 0;
        }
        start = GetTickCount();
        while (1) {
            DWORD elapsed;
            if (g_abort_generation != abort_baseline) {
                send_err(s, "aborted");
                return 0;
            }
            elapsed = GetTickCount() - start;
            if (elapsed >= (DWORD)timeout_ms) {
                send_err(s, "timeout");
                return 0;
            }
            Sleep(50);
            if (capture_thumbnail(current) < 0) continue;
            if (thumbnail_diff_pct(current, baseline) > threshold_pct) {
                BYTE *bmp = NULL;
                DWORD bmp_size = 0;
                char hdr[64];
                int hdr_n;
                if (capture_image(format, &bmp, &bmp_size) < 0) {
                    send_err(s, "capture failed");
                    return 0;
                }
                hdr_n = _snprintf(hdr, sizeof(hdr), "OK %lu %lu\n",
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
        DWORD len;
        BYTE *buf;
        char path[MAX_PATH];
        size_t pathlen;
        if (!space) { send_err(s, "usage: WRITE <path> <length>"); return 0; }
        len = (DWORD)atol(space + 1);
        if (len > MAX_WRITE_BYTES) { send_err(s, "payload too large"); return 0; }
        pathlen = space - arg;
        if (pathlen >= sizeof(path)) { send_err(s, "path too long"); return 0; }
        memcpy(path, arg, pathlen); path[pathlen] = 0;
        buf = (BYTE*)malloc(len ? len : 1);
        if (!buf) { send_err(s, "oom"); return 0; }
        if (len > 0 && recv_n(s, buf, (int)len) < 0) { free(buf); send_err(s, "short read"); return 0; }
        if (write_whole_file(path, buf, len) < 0) { free(buf); send_err(s, "write failed"); return 0; }
        free(buf);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "LIST")) {
        BYTE *buf = NULL; DWORD sz = 0;
        if (!*arg) { send_err(s, "missing path"); return 0; }
        if (list_dir(arg, &buf, &sz) < 0) { send_err(s, "list failed"); return 0; }
        send_ok_data(s, buf, sz);
        free(buf);
        return 0;
    }
    if (!_stricmp(cmd, "STAT")) {
        char type;
        unsigned __int64 size;
        long mtime;
        char rsp[128];
        if (!*arg) { send_err(s, "missing path"); return 0; }
        if (do_stat(arg, &type, &size, &mtime) < 0) { send_err(s, "not found"); return 0; }
        _snprintf(rsp, sizeof(rsp), "OK %c %I64u %ld\n", type, size, mtime);
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
        /* Tab separator if either path has spaces; otherwise plain space. */
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
        press_key_combo(arg);
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
        keybd_event((BYTE)vk, 0, 0, 0);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "KEYUP")) {
        int vk = parse_vk(arg);
        if (vk < 0) { send_err(s, "unknown key"); return 0; }
        keybd_event((BYTE)vk, 0, KEYEVENTF_KEYUP, 0);
        send_ok(s);
        return 0;
    }

    /* ---- Mouse ---- */
    if (!_stricmp(cmd, "MOVE")) {
        int x, y;
        if (sscanf(arg, "%d %d", &x, &y) != 2) { send_err(s, "usage: MOVE x y"); return 0; }
        SetCursorPos(x, y);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "MOVEREL")) {
        int dx, dy;
        POINT p;
        if (sscanf(arg, "%d %d", &dx, &dy) != 2) { send_err(s, "usage: MOVEREL dx dy"); return 0; }
        GetCursorPos(&p);
        SetCursorPos(p.x + dx, p.y + dy);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "CLICK")) {
        do_click(parse_button(arg), 0);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "DCLICK")) {
        do_click(parse_button(arg), 1);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "MDOWN")) {
        do_mdown(parse_button(arg));
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "MUP")) {
        do_mup(parse_button(arg));
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "DRAG")) {
        int x, y, btn = 1;
        char btn_name[16] = "";
        int n = sscanf(arg, "%d %d %15s", &x, &y, btn_name);
        if (n < 2) { send_err(s, "usage: DRAG x y [button]"); return 0; }
        if (n >= 3) btn = parse_button(btn_name);
        do_mdown(btn);
        Sleep(20);
        SetCursorPos(x, y);
        Sleep(20);
        do_mup(btn);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "WHEEL")) {
        int delta = atoi(arg);
        do_wheel(delta);
        send_ok(s);
        return 0;
    }

    /* ---- Process ---- */
    if (!_stricmp(cmd, "EXEC")) {
        DWORD pid = 0;
        char rsp[64];
        if (!*arg) { send_err(s, "missing cmdline"); return 0; }
        if (do_exec(arg, &pid) < 0) { send_err(s, "CreateProcess failed"); return 0; }
        _snprintf(rsp, sizeof(rsp), "OK %lu\n", (unsigned long)pid);
        send_str(s, rsp);
        return 0;
    }
    if (!_stricmp(cmd, "RUN")) {
        DWORD exit_code = 0;
        BYTE *buf = NULL; DWORD sz = 0;
        char hdr[64];
        int n, rc;
        if (!*arg) { send_err(s, "missing cmdline"); return 0; }
        rc = do_run(arg, &exit_code, &buf, &sz);
        if (rc == -3) { send_err(s, "aborted"); return 0; }
        if (rc < 0)   { send_err(s, "CreateProcess failed"); return 0; }
        n = _snprintf(hdr, sizeof(hdr), "OK %lu %lu\n",
                      (unsigned long)exit_code, (unsigned long)sz);
        send_all(s, hdr, n);
        if (sz > 0) send_all(s, (const char*)buf, (int)sz);
        free(buf);
        return 0;
    }
    if (!_stricmp(cmd, "WAIT")) {
        DWORD pid = 0, timeout = INFINITE, exit_code = 0;
        char rsp[64];
        int rc;
        if (sscanf(arg, "%lu %lu", &pid, &timeout) < 1) { send_err(s, "usage: WAIT pid [timeout_ms]"); return 0; }
        rc = do_wait(pid, timeout, &exit_code);
        if (rc == -1) { send_err(s, "OpenProcess failed"); return 0; }
        if (rc == -2) { send_err(s, "timeout"); return 0; }
        if (rc == -3) { send_err(s, "aborted"); return 0; }
        _snprintf(rsp, sizeof(rsp), "OK %lu\n", (unsigned long)exit_code);
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
        /* Chunk into 100 ms slices so ABORT can interrupt and so very long
           SLEEPs don't pin the connection past a panic. */
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
        DWORD len = (DWORD)atol(arg);
        BYTE *buf;
        if (!*arg) { send_err(s, "usage: CLIPSET <length>"); return 0; }
        if (len > MAX_CLIPSET_BYTES) { send_err(s, "payload too large"); return 0; }
        buf = (BYTE*)malloc(len ? len : 1);
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
        HWND hwnd;
        char rsp[64];
        RECT r;
        if (sscanf(arg, "%127[^\r\n]", title) < 1) { send_err(s, "usage: WINFIND title"); return 0; }
        hwnd = find_window_by_title_prefix(title);
        if (!hwnd) { send_err(s, "window not found"); return 0; }
        GetWindowRect(hwnd, &r);
        _snprintf(rsp, sizeof(rsp), "OK %ld %ld %ld %ld\n",
                  r.left, r.top, r.right - r.left, r.bottom - r.top);
        send_str(s, rsp);
        return 0;
    }
    if (!_stricmp(cmd, "WINACTIVE")) {
        HWND hwnd = GetForegroundWindow();
        char title[256] = "";
        RECT r;
        char rsp[512];
        if (!hwnd) { send_err(s, "no active window"); return 0; }
        GetWindowTextA(hwnd, title, sizeof(title));
        GetWindowRect(hwnd, &r);
        _snprintf(rsp, sizeof(rsp),
                  "OK %lu\t%ld\t%ld\t%ld\t%ld\t%s\n",
                  (unsigned long)(ULONG_PTR)hwnd,
                  (long)r.left, (long)r.top,
                  (long)(r.right - r.left), (long)(r.bottom - r.top),
                  title);
        send_str(s, rsp);
        return 0;
    }
    if (!_stricmp(cmd, "WINMOVE")) {
        int x, y;
        char title[128] = "";
        HWND hwnd;
        char rsp[64];
        RECT r;
        if (sscanf(arg, "%d %d %127[^\r\n]", &x, &y, title) < 3) {
            send_err(s, "usage: WINMOVE x y title"); return 0;
        }
        hwnd = find_window_by_title_prefix(title);
        if (!hwnd) { send_err(s, "window not found"); return 0; }
        if (!SetWindowPos(hwnd, HWND_TOP, x, y, 0, 0,
                          SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW)) {
            send_err(s, "SetWindowPos failed"); return 0;
        }
        SetForegroundWindow(hwnd);
        GetWindowRect(hwnd, &r);
        _snprintf(rsp, sizeof(rsp), "OK %ld %ld %ld %ld\n",
                  r.left, r.top, r.right - r.left, r.bottom - r.top);
        send_str(s, rsp);
        return 0;
    }
    if (!_stricmp(cmd, "WINSIZE")) {
        int w, h;
        char title[128] = "";
        HWND hwnd;
        char rsp[64];
        RECT r;
        if (sscanf(arg, "%d %d %127[^\r\n]", &w, &h, title) < 3) {
            send_err(s, "usage: WINSIZE w h title"); return 0;
        }
        hwnd = find_window_by_title_prefix(title);
        if (!hwnd) { send_err(s, "window not found"); return 0; }
        if (!SetWindowPos(hwnd, HWND_TOP, 0, 0, w, h,
                          SWP_NOMOVE | SWP_NOZORDER | SWP_SHOWWINDOW)) {
            send_err(s, "SetWindowPos failed"); return 0;
        }
        GetWindowRect(hwnd, &r);
        _snprintf(rsp, sizeof(rsp), "OK %ld %ld %ld %ld\n",
                  r.left, r.top, r.right - r.left, r.bottom - r.top);
        send_str(s, rsp);
        return 0;
    }
    if (!_stricmp(cmd, "WINFOCUS") || !_stricmp(cmd, "WINCLOSE") ||
        !_stricmp(cmd, "WINMIN") || !_stricmp(cmd, "WINMAX") ||
        !_stricmp(cmd, "WINRESTORE")) {
        HWND hwnd;
        if (!*arg) { send_err(s, "usage: <verb> title"); return 0; }
        hwnd = find_window_by_title_prefix(arg);
        if (!hwnd) { send_err(s, "window not found"); return 0; }
        if (!_stricmp(cmd, "WINFOCUS")) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        } else if (!_stricmp(cmd, "WINCLOSE")) {
            PostMessageA(hwnd, WM_CLOSE, 0, 0);
        } else if (!_stricmp(cmd, "WINMIN")) {
            ShowWindow(hwnd, SW_MINIMIZE);
        } else if (!_stricmp(cmd, "WINMAX")) {
            ShowWindow(hwnd, SW_MAXIMIZE);
        } else {
            ShowWindow(hwnd, SW_RESTORE);
        }
        send_ok(s);
        return 0;
    }

    /* ---- System ---- */
    if (!_stricmp(cmd, "ENV")) {
        if (*arg) {
            char val[4096];
            DWORD n = GetEnvironmentVariableA(arg, val, sizeof(val));
            char rsp[4200];
            if (n == 0) { send_err(s, "not set"); return 0; }
            if (n >= sizeof(val)) { send_err(s, "value too long"); return 0; }
            _snprintf(rsp, sizeof(rsp), "OK %s\n", val);
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
        char rsp[32];
        if (get_idle_seconds(&secs) < 0) { send_err(s, "idle failed"); return 0; }
        _snprintf(rsp, sizeof(rsp), "OK %lu\n", (unsigned long)secs);
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

    /* ---- Power (gated by g_enable_power) ---- */
    if (!_stricmp(cmd, "LOGOFF") || !_stricmp(cmd, "REBOOT") || !_stricmp(cmd, "SHUTDOWN")) {
        UINT flags;
        if (!g_enable_power) { send_err(s, "power verbs disabled"); return 0; }
        if (!_stricmp(cmd, "LOGOFF"))      flags = EWX_LOGOFF;
        else if (!_stricmp(cmd, "REBOOT")) flags = EWX_REBOOT;
        else                               flags = EWX_POWEROFF;
        if (flags != EWX_LOGOFF) enable_shutdown_privilege();
        send_ok(s);
        if (!ExitWindowsEx(flags, 0)) {
            fprintf(stderr, "ExitWindowsEx failed: %lu\n", GetLastError());
        }
        return 1;
    }

    send_err(s, "unknown command");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Server main                                                         */
/* ------------------------------------------------------------------ */

/* Per-thread worker. Owns one accepted client socket; runs the dispatch
   loop with the SEH crash barrier; cleans up its connection slot on exit. */
typedef struct {
    SOCKET sock;
    char addr[64];  /* dotted-quad of the peer for logging */
} conn_args;

static unsigned __stdcall connection_worker(void *arg) {
    conn_args *ca = (conn_args*)arg;
    SOCKET c = ca->sock;
    char addr[64];
    strncpy(addr, ca->addr, sizeof(addr) - 1);
    addr[sizeof(addr) - 1] = 0;
    free(ca);

    printf("[+] %s connected\n", addr); fflush(stdout);

    while (1) {
        char line[LINEBUF];
        int n = recv_line(c, line, sizeof(line));
        int close_conn = 0;
        DWORD exc_code = 0;
        if (n < 0) break;
        if (n == 0) continue;
        printf("[%s] > %s\n", addr, line); fflush(stdout);
        /* SEH catches AVs, divide-by-zero, stack overflow inside the
           handler — turn a process-killing fault into a per-connection
           failure. Other connections' threads carry on. */
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

    /* Decrement live-connection count. Only the *last* disconnecting
       connection runs cleanup_input_state — otherwise we'd stomp on input
       state that another live connection legitimately holds (KEYDOWN ctrl,
       MDOWN left, etc.). */
    EnterCriticalSection(&g_conn_lock);
    g_active_connections--;
    int last = (g_active_connections == 0);
    LeaveCriticalSection(&g_conn_lock);
    if (last) cleanup_input_state();

    printf("[-] %s disconnected\n", addr); fflush(stdout);
    return 0;
}

static void serve(SOCKET listener) {
    while (1) {
        struct sockaddr_in cli;
        int cli_len = sizeof(cli);
        SOCKET c = accept(listener, (struct sockaddr*)&cli, &cli_len);
        if (c == INVALID_SOCKET) continue;

        /* Cap concurrent connections. Reject extras with a polite ERR so
           clients can retry — better than queueing and stalling. */
        EnterCriticalSection(&g_conn_lock);
        int admit = (g_active_connections < MAX_CONNECTIONS);
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
        strncpy(ca->addr, inet_ntoa(cli.sin_addr), sizeof(ca->addr) - 1);
        ca->addr[sizeof(ca->addr) - 1] = 0;

        unsigned long h = _beginthreadex(NULL, 0, connection_worker, ca, 0, NULL);
        if (h == 0) {
            EnterCriticalSection(&g_conn_lock);
            g_active_connections--;
            LeaveCriticalSection(&g_conn_lock);
            send_str(c, "ERR thread\n");
            closesocket(c);
            free(ca);
            continue;
        }
        /* Detached worker: close the thread handle now; the OS reclaims when
           the thread exits. We never join — connections are independent. */
        CloseHandle((HANDLE)h);
    }
}

static int run_server(void) {
    WSADATA wsa;
    SOCKET ls;
    struct sockaddr_in addr;
    int opt = 1;

    InitializeCriticalSection(&g_proc_lock);
    InitializeCriticalSection(&g_conn_lock);
    init_gdiplus();  /* Best-effort; PNG falls back to ERR if gdiplus.dll missing. */

    if (WSAStartup(MAKEWORD(1, 1), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n"); return 1;
    }
    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == INVALID_SOCKET) { fprintf(stderr, "socket failed\n"); return 1; }
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)g_port);
    if (bind(ls, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "bind failed (port %d in use?)\n", g_port);
        return 1;
    }
    /* listen backlog of MAX_CONNECTIONS so SYN-burst arrivals queue cleanly
       up to the cap; further pile up at TCP layer until accept catches up. */
    listen(ls, MAX_CONNECTIONS);
    printf("agent-remote-hands (windows-nt) listening on 0.0.0.0:%d "
           "(power=%s, max_connections=%d)\n",
           g_port, g_enable_power ? "on" : "off", MAX_CONNECTIONS);
    fflush(stdout);
    serve(ls);
    closesocket(ls);
    WSACleanup();
    cleanup_gdiplus();
    DeleteCriticalSection(&g_proc_lock);
    DeleteCriticalSection(&g_conn_lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Service mode                                                       */
/* ------------------------------------------------------------------ */

static VOID WINAPI svc_ctrl(DWORD ctrl) {
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        g_svc_status.dwCurrentState = SERVICE_STOP_PENDING;
        g_svc_status.dwWaitHint     = 1000;
        SetServiceStatus(g_svc_handle, &g_svc_status);
        /* The accept() loop has no clean shutdown signal; rely on ExitProcess
           to tear down. SCM marks us stopped once the process exits. */
        ExitProcess(0);
    }
}

static VOID WINAPI svc_main(DWORD argc, LPSTR *argv) {
    (void)argc; (void)argv;
    g_svc_handle = RegisterServiceCtrlHandlerA(SERVICE_NAME, svc_ctrl);
    if (!g_svc_handle) return;
    ZeroMemory(&g_svc_status, sizeof(g_svc_status));
    g_svc_status.dwServiceType  = SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS;
    g_svc_status.dwCurrentState = SERVICE_RUNNING;
    g_svc_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(g_svc_handle, &g_svc_status);
    run_server();
    g_svc_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_svc_handle, &g_svc_status);
}

static int do_install(void) {
    SC_HANDLE scm, svc;
    char path[MAX_PATH];
    char binpath[MAX_PATH + 32];
    SERVICE_FAILURE_ACTIONSA sfa;
    SC_ACTION actions[3];

    if (GetModuleFileNameA(NULL, path, sizeof(path)) == 0) return -1;
    /* Quote the path so spaces in install dir don't break SCM parsing.
       Append --service so the binary, on next launch, enters service mode. */
    _snprintf(binpath, sizeof(binpath), "\"%s\" --service", path);

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) return -1;
    svc = CreateServiceA(scm, SERVICE_NAME, SERVICE_DISPLAY,
                         SERVICE_ALL_ACCESS,
                         SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS,
                         SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                         binpath, NULL, NULL, NULL, NULL, NULL);
    if (!svc) { CloseServiceHandle(scm); return -1; }

    /* Auto-restart on crash: 1s, 5s, 10s, then give up for 24h. */
    actions[0].Type = SC_ACTION_RESTART; actions[0].Delay = 1000;
    actions[1].Type = SC_ACTION_RESTART; actions[1].Delay = 5000;
    actions[2].Type = SC_ACTION_RESTART; actions[2].Delay = 10000;
    sfa.dwResetPeriod = 86400;
    sfa.lpRebootMsg   = NULL;
    sfa.lpCommand     = NULL;
    sfa.cActions      = 3;
    sfa.lpsaActions   = actions;
    ChangeServiceConfig2A(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &sfa);

    StartServiceA(svc, 0, NULL);  /* best-effort; user can ignore failure */
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

static int do_uninstall(void) {
    SC_HANDLE scm, svc;
    SERVICE_STATUS st;
    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return -1;
    svc = OpenServiceA(scm, SERVICE_NAME, SERVICE_STOP | DELETE);
    if (!svc) { CloseServiceHandle(scm); return -1; }
    ControlService(svc, SERVICE_CONTROL_STOP, &st);  /* best-effort stop */
    if (!DeleteService(svc)) {
        CloseServiceHandle(svc); CloseServiceHandle(scm); return -1;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

static void print_usage(void) {
    printf("agent-remote-hands (windows-nt)\n"
           "\n"
           "Usage:\n"
           "  remote-hands.exe [<port>]   run in foreground (default port 8765)\n"
           "  remote-hands.exe --install     register and start as Windows service\n"
           "  remote-hands.exe --uninstall   stop and remove the service\n"
           "  remote-hands.exe --service     internal: invoked by SCM\n"
           "\n"
           "Env: REMOTE_HANDS_PORT, REMOTE_HANDS_POWER (=1 to enable LOGOFF/REBOOT/SHUTDOWN)\n");
}

int main(int argc, char **argv) {
    int i;
    /* REMOTE_HANDS_PORT preferred; VM_AGENT_PORT kept as back-compat alias. */
    char *env_port  = getenv("REMOTE_HANDS_PORT");
    char *env_power = getenv("REMOTE_HANDS_POWER");
    if (!env_port) env_port = getenv("VM_AGENT_PORT");
    if (env_port) g_port = atoi(env_port);
    if (env_power && atoi(env_power) != 0) g_enable_power = 1;

    /* Suppress modal error dialogs (WER, GP fault box, missing-disk prompts)
       — on a headless VM these freeze the agent waiting for human input. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--install") || !_stricmp(argv[i], "/install")) {
            if (do_install() < 0) {
                fprintf(stderr, "install failed: %lu (run elevated?)\n", GetLastError());
                return 1;
            }
            printf("Service '%s' installed and started.\n", SERVICE_NAME);
            return 0;
        }
        if (!strcmp(argv[i], "--uninstall") || !_stricmp(argv[i], "/uninstall")) {
            if (do_uninstall() < 0) {
                fprintf(stderr, "uninstall failed: %lu (run elevated?)\n", GetLastError());
                return 1;
            }
            printf("Service '%s' removed.\n", SERVICE_NAME);
            return 0;
        }
        if (!strcmp(argv[i], "--service")) {
            SERVICE_TABLE_ENTRYA st[2];
            st[0].lpServiceName = SERVICE_NAME;
            st[0].lpServiceProc = svc_main;
            st[1].lpServiceName = NULL;
            st[1].lpServiceProc = NULL;
            if (!StartServiceCtrlDispatcherA(st)) {
                fprintf(stderr, "StartServiceCtrlDispatcher failed: %lu\n", GetLastError());
                return 1;
            }
            return 0;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h") || !strcmp(argv[i], "/?")) {
            print_usage();
            return 0;
        }
        /* Bare numeric arg overrides port (back-compat with `remote-hands.exe 9999`). */
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

/* wgc.cpp — Windows.Graphics.Capture implementation. See wgc.h for the
 * shape and rationale. C++/WinRT for the WGC bindings; raw D3D11 + DXGI
 * for the capture-to-CPU readback path. Each capture is synchronous: set
 * up a single-frame pool, start the session, wait on a Win32 event for
 * the FrameArrived callback, copy the texture into a CPU staging texture,
 * Map() it, and blit pixels into a CreateDIBSection HBITMAP.
 *
 * Per-call sessions add ~50–100 ms overhead vs. BitBlt; for the once-per-
 * SHOT case that's invisible. WATCH benefits less per-frame because each
 * call still rebuilds the session — see TODO.md for the streaming path
 * that holds a session open across a WATCH duration.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <inspectable.h>
#include <wrl/client.h>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include "wgc.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowsapp.lib")

using Microsoft::WRL::ComPtr;
namespace WGC = winrt::Windows::Graphics::Capture;
namespace WGD = winrt::Windows::Graphics::DirectX;

/* ----- Globals: D3D device + WinRT device, set up once. ----- */
static ComPtr<ID3D11Device> g_d3d_device;
static ComPtr<ID3D11DeviceContext> g_d3d_ctx;
static WGD::Direct3D11::IDirect3DDevice g_winrt_device{ nullptr };
static int g_available = 0;
static CRITICAL_SECTION g_wgc_lock;
static int g_wgc_lock_initialised = 0;

/* IDirect3DDevice <-> ID3D11Device interop. Declared by
   <windows.graphics.directx.direct3d11.interop.h>; redeclared here so the
   header isn't strictly required for callers. */
extern "C" {
HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(
    ::IDXGIDevice *dxgiDevice, ::IInspectable **graphicsDevice);
}

struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
IDirect3DDxgiInterfaceAccess : ::IUnknown {
    virtual HRESULT __stdcall GetInterface(REFIID iid, void **p) = 0;
};

extern "C" int wgc_available(void) { return g_available; }

extern "C" int wgc_init(void) {
    if (g_available) return 1;
    if (!g_wgc_lock_initialised) {
        InitializeCriticalSection(&g_wgc_lock);
        g_wgc_lock_initialised = 1;
    }

    /* Initialise apartment for *this* thread (run_server's thread). Worker
       threads must call wgc_thread_init() themselves. */
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE) {
        /* Already STA-initialised on this thread (e.g. by Task Scheduler
           install path before run_server). Tolerable — RoInitialize on a
           different thread sets MTA fresh, and our worker threads are
           those different threads. */
    } else if (FAILED(hr)) {
        return 0;
    }

    /* IsSupported() returns false on Win 7 / 8 / early 10. */
    try {
        if (!WGC::GraphicsCaptureSession::IsSupported()) return 0;
    } catch (...) {
        return 0;
    }

    /* Create the D3D11 device. BGRA support is required for WGC. */
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                           fl, ARRAYSIZE(fl), D3D11_SDK_VERSION,
                           g_d3d_device.GetAddressOf(), nullptr,
                           g_d3d_ctx.GetAddressOf());
    if (FAILED(hr)) {
        /* Fall back to WARP (software) — works on locked-down VMs without
           hardware acceleration. */
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                               fl, ARRAYSIZE(fl), D3D11_SDK_VERSION,
                               g_d3d_device.GetAddressOf(), nullptr,
                               g_d3d_ctx.GetAddressOf());
        if (FAILED(hr)) return 0;
    }

    /* Wrap the D3D11 device as a WinRT IDirect3DDevice. */
    ComPtr<IDXGIDevice> dxgi;
    hr = g_d3d_device.As(&dxgi);
    if (FAILED(hr)) { g_d3d_device.Reset(); g_d3d_ctx.Reset(); return 0; }
    ComPtr<::IInspectable> insp;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), insp.GetAddressOf());
    if (FAILED(hr)) { g_d3d_device.Reset(); g_d3d_ctx.Reset(); return 0; }
    try {
        g_winrt_device = insp.Get()->QueryInterface(
            winrt::guid_of<WGD::Direct3D11::IDirect3DDevice>(),
            winrt::put_abi(g_winrt_device)) == S_OK
            ? g_winrt_device
            : WGD::Direct3D11::IDirect3DDevice{ nullptr };
    } catch (...) {
        g_d3d_device.Reset(); g_d3d_ctx.Reset();
        return 0;
    }
    if (!g_winrt_device) { g_d3d_device.Reset(); g_d3d_ctx.Reset(); return 0; }

    g_available = 1;
    return 1;
}

extern "C" void wgc_shutdown(void) {
    if (!g_available) return;
    g_winrt_device = nullptr;
    g_d3d_ctx.Reset();
    g_d3d_device.Reset();
    g_available = 0;
    if (g_wgc_lock_initialised) {
        DeleteCriticalSection(&g_wgc_lock);
        g_wgc_lock_initialised = 0;
    }
}

extern "C" void wgc_thread_init(void) {
    if (!g_available) return;
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    /* RPC_E_CHANGED_MODE means another component already set STA on this
       thread — fine, our calls work in either mode for the purposes of
       capture. S_FALSE means already initialised; harmless. */
    (void)hr;
}

extern "C" void wgc_thread_uninit(void) {
    if (!g_available) return;
    RoUninitialize();
}

/* ------------------------------------------------------------------ */
/* Capture core: D3D surface → HBITMAP readback.                      */
/* ------------------------------------------------------------------ */

/* Reads the pixels of an IDirect3DSurface (BGRA, top-down) into a
   freshly-created CreateDIBSection HBITMAP. Caller DeleteObject's. Must
   be called with g_wgc_lock held — the shared D3D context isn't safe for
   concurrent use. */
static HBITMAP surface_to_hbitmap(WGD::Direct3D11::IDirect3DSurface const &surface) {
    if (!surface) return NULL;
    HBITMAP result = NULL;
    try {
        ComPtr<IDirect3DDxgiInterfaceAccess> access;
        surface.as<IDirect3DDxgiInterfaceAccess>().copy_to(access.GetAddressOf());
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(access->GetInterface(__uuidof(ID3D11Texture2D),
                                        (void**)tex.GetAddressOf())))
            return NULL;

        D3D11_TEXTURE2D_DESC desc;
        tex->GetDesc(&desc);

        D3D11_TEXTURE2D_DESC stage = desc;
        stage.Usage = D3D11_USAGE_STAGING;
        stage.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stage.BindFlags = 0;
        stage.MiscFlags = 0;

        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(g_d3d_device->CreateTexture2D(&stage, NULL, staging.GetAddressOf())))
            return NULL;
        g_d3d_ctx->CopyResource(staging.Get(), tex.Get());
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(g_d3d_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return NULL;

        int w = (int)desc.Width;
        int h = (int)desc.Height;
        BITMAPINFO bi;
        ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;  /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC hdc = GetDC(NULL);
        void *pvBits = NULL;
        HBITMAP hbm = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &pvBits, NULL, 0);
        ReleaseDC(NULL, hdc);
        if (hbm && pvBits) {
            int dst_stride = w * 4;
            int src_stride = (int)mapped.RowPitch;
            if (dst_stride == src_stride) {
                memcpy(pvBits, mapped.pData, (size_t)dst_stride * h);
            } else {
                for (int y = 0; y < h; y++) {
                    memcpy((BYTE*)pvBits + (size_t)dst_stride * y,
                           (BYTE*)mapped.pData + (size_t)src_stride * y,
                           dst_stride);
                }
            }
            result = hbm;
        } else if (hbm) {
            DeleteObject(hbm);
        }
        g_d3d_ctx->Unmap(staging.Get(), 0);
    } catch (...) {}
    return result;
}

/* One-shot per-item capture: build a single-frame pool, start session, wait
   for the frame, read it back, tear down. Used for SHOT/SHOTWIN/SHOTRECT.
   For long-running streams (WATCH/WAITFOR) use the session API instead. */
static HBITMAP capture_item_to_hbitmap(WGC::GraphicsCaptureItem const &item) {
    if (!item) return NULL;
    HBITMAP result = NULL;
    HANDLE frame_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!frame_event) return NULL;

    EnterCriticalSection(&g_wgc_lock);
    try {
        auto size = item.Size();
        if (size.Width <= 0 || size.Height <= 0) {
            LeaveCriticalSection(&g_wgc_lock);
            CloseHandle(frame_event);
            return NULL;
        }
        auto pool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(
            g_winrt_device, WGD::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            1, size);
        auto session = pool.CreateCaptureSession(item);
        auto token = pool.FrameArrived([frame_event](auto&&, auto&&) {
            SetEvent(frame_event);
        });
        try { session.IsCursorCaptureEnabled(true); } catch (...) {}
        try { session.IsBorderRequired(false); } catch (...) {}
        session.StartCapture();
        if (WaitForSingleObject(frame_event, 1500) == WAIT_OBJECT_0) {
            auto frame = pool.TryGetNextFrame();
            if (frame) {
                result = surface_to_hbitmap(frame.Surface());
                frame.Close();
            }
        }
        pool.FrameArrived(token);
        session.Close();
        pool.Close();
    } catch (...) {}
    LeaveCriticalSection(&g_wgc_lock);
    CloseHandle(frame_event);
    return result;
}

/* ------------------------------------------------------------------ */
/* Per-monitor capture, used by primary / virtual-screen / rect paths. */
/* ------------------------------------------------------------------ */

static HBITMAP wgc_capture_monitor(HMONITOR mon) {
    if (!g_available || !mon) return NULL;
    auto interop = winrt::get_activation_factory<
        WGC::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    WGC::GraphicsCaptureItem item{ nullptr };
    HRESULT hr = interop->CreateForMonitor(
        mon, winrt::guid_of<WGC::GraphicsCaptureItem>(),
        winrt::put_abi(item));
    if (FAILED(hr) || !item) return NULL;
    return capture_item_to_hbitmap(item);
}

extern "C" HBITMAP wgc_capture_primary_monitor(void) {
    return wgc_capture_monitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
}

extern "C" HBITMAP wgc_capture_window(HWND hwnd) {
    if (!g_available || !hwnd) return NULL;
    auto interop = winrt::get_activation_factory<
        WGC::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    WGC::GraphicsCaptureItem item{ nullptr };
    HRESULT hr = interop->CreateForWindow(
        hwnd, winrt::guid_of<WGC::GraphicsCaptureItem>(),
        winrt::put_abi(item));
    if (FAILED(hr) || !item) return NULL;
    return capture_item_to_hbitmap(item);
}

/* ------------------------------------------------------------------ */
/* Multi-monitor virtual-screen capture: enumerate, capture each,     */
/* stitch into a single HBITMAP at the virtual-screen rect.           */
/* ------------------------------------------------------------------ */

struct mon_entry { HMONITOR mon; RECT rect; };

static BOOL CALLBACK enum_monitor_cb(HMONITOR mon, HDC, LPRECT rect, LPARAM lp) {
    auto *list = (std::vector<mon_entry>*)lp;
    list->push_back({mon, *rect});
    return TRUE;
}

extern "C" HBITMAP wgc_capture_virtual_screen(void) {
    if (!g_available) return NULL;
    std::vector<mon_entry> monitors;
    EnumDisplayMonitors(NULL, NULL, enum_monitor_cb, (LPARAM)&monitors);
    if (monitors.empty()) return NULL;
    if (monitors.size() == 1) return wgc_capture_monitor(monitors[0].mon);

    int vsx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vsy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vsw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vsh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vsw <= 0 || vsh <= 0) return NULL;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = vsw;
    bi.bmiHeader.biHeight = -vsh;  /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC hdc_screen = GetDC(NULL);
    void *pvBits = NULL;
    HBITMAP composed = CreateDIBSection(hdc_screen, &bi, DIB_RGB_COLORS, &pvBits, NULL, 0);
    ReleaseDC(NULL, hdc_screen);
    if (!composed || !pvBits) {
        if (composed) DeleteObject(composed);
        return NULL;
    }
    /* Zero gaps for non-rectangular monitor arrangements. */
    memset(pvBits, 0, (size_t)vsw * vsh * 4);

    HDC hdc_dst = CreateCompatibleDC(NULL);
    HGDIOBJ old_dst = SelectObject(hdc_dst, composed);
    for (auto const &m : monitors) {
        HBITMAP mon_bmp = wgc_capture_monitor(m.mon);
        if (!mon_bmp) continue;
        HDC hdc_src = CreateCompatibleDC(NULL);
        HGDIOBJ old_src = SelectObject(hdc_src, mon_bmp);
        BitBlt(hdc_dst, m.rect.left - vsx, m.rect.top - vsy,
               m.rect.right - m.rect.left, m.rect.bottom - m.rect.top,
               hdc_src, 0, 0, SRCCOPY);
        SelectObject(hdc_src, old_src);
        DeleteDC(hdc_src);
        DeleteObject(mon_bmp);
    }
    SelectObject(hdc_dst, old_dst);
    DeleteDC(hdc_dst);
    return composed;
}

/* ------------------------------------------------------------------ */
/* Rect capture: WGC the parent monitor, crop to requested rect.      */
/* ------------------------------------------------------------------ */

extern "C" HBITMAP wgc_capture_rect(int sx, int sy, int w, int h) {
    if (!g_available || w <= 0 || h <= 0) return NULL;
    POINT pt{ sx, sy };
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
    if (!mon) return NULL;
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(mon, &mi)) return NULL;
    /* Reject cross-monitor rects — caller should use virtual-screen + crop
       (or BitBlt) for those. */
    if (sx < mi.rcMonitor.left || sy < mi.rcMonitor.top ||
        sx + w > mi.rcMonitor.right || sy + h > mi.rcMonitor.bottom) {
        return NULL;
    }
    HBITMAP mon_bmp = wgc_capture_monitor(mon);
    if (!mon_bmp) return NULL;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC hdc_screen = GetDC(NULL);
    void *pvBits = NULL;
    HBITMAP cropped = CreateDIBSection(hdc_screen, &bi, DIB_RGB_COLORS, &pvBits, NULL, 0);
    ReleaseDC(NULL, hdc_screen);
    if (!cropped) { DeleteObject(mon_bmp); return NULL; }

    HDC hdc_dst = CreateCompatibleDC(NULL);
    HGDIOBJ old_dst = SelectObject(hdc_dst, cropped);
    HDC hdc_src = CreateCompatibleDC(NULL);
    HGDIOBJ old_src = SelectObject(hdc_src, mon_bmp);
    BitBlt(hdc_dst, 0, 0, w, h, hdc_src,
           sx - mi.rcMonitor.left, sy - mi.rcMonitor.top, SRCCOPY);
    SelectObject(hdc_src, old_src);
    DeleteDC(hdc_src);
    SelectObject(hdc_dst, old_dst);
    DeleteDC(hdc_dst);
    DeleteObject(mon_bmp);
    return cropped;
}

/* ------------------------------------------------------------------ */
/* Streaming session: hold a WGC session open across many frames.     */
/* ------------------------------------------------------------------ */

struct wgc_session {
    WGC::GraphicsCaptureItem item{ nullptr };
    WGC::Direct3D11CaptureFramePool pool{ nullptr };
    WGC::GraphicsCaptureSession session{ nullptr };
    winrt::event_token frame_token{};
    HANDLE frame_event = NULL;
    int started = 0;
};

static struct wgc_session *open_session_from_item(WGC::GraphicsCaptureItem item) {
    if (!g_available || !item) return NULL;
    auto *s = new wgc_session;
    s->item = item;
    s->frame_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!s->frame_event) { delete s; return NULL; }
    try {
        auto size = item.Size();
        if (size.Width <= 0 || size.Height <= 0) throw winrt::hresult_invalid_argument();
        /* 2-frame pool gives the producer one frame ahead of the consumer
           without queueing more than one stale frame. */
        s->pool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(
            g_winrt_device, WGD::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2, size);
        s->session = s->pool.CreateCaptureSession(item);
        HANDLE ev = s->frame_event;
        s->frame_token = s->pool.FrameArrived([ev](auto&&, auto&&) {
            SetEvent(ev);
        });
        try { s->session.IsCursorCaptureEnabled(true); } catch (...) {}
        try { s->session.IsBorderRequired(false); } catch (...) {}
        s->session.StartCapture();
        s->started = 1;
        return s;
    } catch (...) {
        if (s->frame_event) CloseHandle(s->frame_event);
        delete s;
        return NULL;
    }
}

extern "C" struct wgc_session *wgc_session_open_primary(void) {
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (!mon) return NULL;
    auto interop = winrt::get_activation_factory<
        WGC::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    WGC::GraphicsCaptureItem item{ nullptr };
    if (FAILED(interop->CreateForMonitor(mon,
        winrt::guid_of<WGC::GraphicsCaptureItem>(), winrt::put_abi(item))))
        return NULL;
    return open_session_from_item(item);
}

extern "C" struct wgc_session *wgc_session_open_window(HWND hwnd) {
    if (!hwnd) return NULL;
    auto interop = winrt::get_activation_factory<
        WGC::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    WGC::GraphicsCaptureItem item{ nullptr };
    if (FAILED(interop->CreateForWindow(hwnd,
        winrt::guid_of<WGC::GraphicsCaptureItem>(), winrt::put_abi(item))))
        return NULL;
    return open_session_from_item(item);
}

extern "C" HBITMAP wgc_session_get_frame(struct wgc_session *s) {
    if (!s || !s->started) return NULL;
    HBITMAP result = NULL;
    EnterCriticalSection(&g_wgc_lock);
    try {
        DWORD wr = WaitForSingleObject(s->frame_event, 1500);
        if (wr == WAIT_OBJECT_0) {
            ResetEvent(s->frame_event);
            /* Drain the pool: take the latest frame and discard older ones,
               so a slow consumer doesn't replay a backlog of stale frames. */
            WGC::Direct3D11CaptureFrame frame{ nullptr };
            while (true) {
                auto next = s->pool.TryGetNextFrame();
                if (!next) break;
                if (frame) frame.Close();
                frame = next;
            }
            if (frame) {
                result = surface_to_hbitmap(frame.Surface());
                frame.Close();
            }
        }
    } catch (...) {}
    LeaveCriticalSection(&g_wgc_lock);
    return result;
}

extern "C" void wgc_session_close(struct wgc_session *s) {
    if (!s) return;
    try {
        if (s->pool) s->pool.FrameArrived(s->frame_token);
        if (s->session) s->session.Close();
        if (s->pool) s->pool.Close();
    } catch (...) {}
    if (s->frame_event) CloseHandle(s->frame_event);
    delete s;
}

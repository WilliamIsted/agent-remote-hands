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

// windows-legacy OCR seam.
// Windows.Media.Ocr is gated on >= Windows 8.1 RTM (OcrEngine shipped in 8.1).
// combase.dll (Win8+) provides RoInitialize / RoGetActivationFactory /
// WindowsCreate/DeleteString — all loaded at runtime via GetProcAddress so
// the binary has no hard PE imports to WinRT DLLs and loads cleanly on XP/Vista/7.
//
// All WinRT interfaces are declared as raw COM vtable structs. No C++/WinRT
// headers or WRL headers are required beyond the ComPtr shim in shim/.
// IMemoryBufferByteAccess is declared inline (stable IID, no MemoryBuffer.h).
//
// init_ocr() is called from main() before CoInitializeEx (which runs later in
// ComInit). RoInitialize(RO_INIT_SINGLETHREADED=0) sets up an STA; the
// subsequent CoInitializeEx(COINIT_APARTMENTTHREADED) returns S_FALSE (same
// mode, already initialised — not an error).

#include "platform.hpp"

#include <objbase.h>        // CoInitializeEx, CreateStreamOnHGlobal, IStream
#include <versionhelpers.h> // IsWindows8Point1OrGreater

#include <algorithm>
#include <climits>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

// GdiplusTypes.h uses bare min()/max(); with NOMINMAX the Win32 macros are
// suppressed, so promote std::min/max into global scope before the include.
using std::min;
using std::max;
#include <gdiplus.h>        // image decode (decode_bytes_to_frame)

// ---------------------------------------------------------------------------
// HSTRING forward-declaration.
//
// We avoid including <winstring.h> here because it declares WindowsCreateString
// etc. with __declspec(dllimport), which would inject hard PE imports for those
// symbols even if we never call them directly. We load them at runtime via
// GetProcAddress instead.
struct HSTRING__;
typedef HSTRING__* HSTRING;

// ---------------------------------------------------------------------------
// WinRT interface shims.
//
// All interfaces inherit from IInspectable (vtable[3-5]) which inherits from
// IUnknown (vtable[0-2]). Custom verbs start at vtable[6].
// Placed at global scope so the compiler emits exactly one vtable per TU.

// IInspectable ---------------------------------------------------------------
#ifndef __IInspectable_INTERFACE_DEFINED__
#define __IInspectable_INTERFACE_DEFINED__
MIDL_INTERFACE("AF86E2E0-B12D-4c6a-9C5A-D7AA65101E90")
IInspectable : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetIids(ULONG*, IID**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTrustLevel(int*) = 0;
};
#endif

// IActivationFactory ---------------------------------------------------------
#ifndef __IActivationFactory_INTERFACE_DEFINED__
#define __IActivationFactory_INTERFACE_DEFINED__
MIDL_INTERFACE("00000035-0000-0000-C000-000000000046")
IActivationFactory : public IInspectable {
public:
    virtual HRESULT STDMETHODCALLTYPE ActivateInstance(IInspectable**) = 0;
};
#endif

// IAsyncInfo stub (only get_Status + Close needed for await_op) ---------------
enum class AsyncStatus : int { Started = 0, Completed = 1, Canceled = 2, Error = 3 };

struct IAsyncInfoShim : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(AsyncStatus*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

// Generic IAsyncOperation<T*> shim — GetResults always at vtable[8] ----------
struct IAsyncOpResult : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(void**) = 0;
};

// Data structures used by OCR interfaces ------------------------------------
struct WinRTRect             { float X, Y, Width, Height; };
struct BitmapPlaneDescription { INT32 StartIndex, Width, Height, Stride; };

// OCR interfaces -------------------------------------------------------------
// GUIDs from Win10 SDK 19041 winrt/windows.media.ocr.h

struct IOcrWord : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_BoundingRect(WinRTRect*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Text(HSTRING*) = 0;
};

struct IOcrLine : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Words(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Text(HSTRING*) = 0;
};

struct IOcrResult : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Lines(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TextAngle(void**) = 0;  // IReference<double>*
    virtual HRESULT STDMETHODCALLTYPE get_Text(HSTRING*) = 0;
};

struct IOcrEngine : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RecognizeAsync(IUnknown* bitmap, void** async_op) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RecognizerLanguage(void**) = 0;  // ILanguage*
};

struct ILanguage : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_LanguageTag(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DisplayName(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_NativeName(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Script(HSTRING*) = 0;
};

struct IOcrEngineStatics : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_MaxImageDimension(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AvailableRecognizerLanguages(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsLanguageSupported(ILanguage*, BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryCreateFromLanguage(ILanguage*, IOcrEngine**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryCreateFromUserProfileLanguages(IOcrEngine**) = 0;
};

// Imaging interfaces ---------------------------------------------------------
// GUIDs from Win10 SDK 19041 winrt/windows.graphics.imaging.h

struct IBitmapBuffer : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetPlaneCount(INT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPlaneDescription(INT32, BitmapPlaneDescription*) = 0;
};

struct ISoftwareBitmap : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_BitmapPixelFormat(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BitmapAlphaMode(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PixelWidth(INT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PixelHeight(INT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DpiX(DOUBLE) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DpiX(DOUBLE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DpiY(DOUBLE) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DpiY(DOUBLE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE LockBuffer(int mode, IBitmapBuffer**) = 0;  // vtable[15]
    virtual HRESULT STDMETHODCALLTYPE CopyTo(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CopyFromBuffer(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CopyToBuffer(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetReadOnlyView(ISoftwareBitmap**) = 0;
};

struct ISoftwareBitmapFactory : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Create(
        int format, INT32 w, INT32 h, ISoftwareBitmap**) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateWithAlpha(
        int format, INT32 w, INT32 h, int alpha, ISoftwareBitmap**) = 0;
};

// IMemoryBuffer shim — QI target from IBitmapBuffer --------------------------
// IID: fbc4dd2a-245b-11e4-af98-689423260cf8
struct IMemoryBufferShim : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateReference(IUnknown** reference) = 0;
};

// IMemoryBufferByteAccess — stable ABI, declared inline -----------------------
// IID: 5b0d3235-4dba-4d44-865e-8f1d0e4fd04d
#ifndef __IMemoryBufferByteAccess_INTERFACE_DEFINED__
#define __IMemoryBufferByteAccess_INTERFACE_DEFINED__
MIDL_INTERFACE("5b0d3235-4dba-4d44-865e-8f1d0e4fd04d")
IMemoryBufferByteAccess : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(BYTE** value, UINT32* capacity) = 0;
};
#endif

// IVectorView shims (vtable: GetAt[6], get_Size[7], IndexOf[8], GetMany[9]) --
struct IOcrLineVec : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32, IOcrLine**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE IndexOf(IOcrLine*, UINT32*, int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(UINT32, UINT32, IOcrLine**, UINT32*) = 0;
};

struct IOcrWordVec : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32, IOcrWord**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE IndexOf(IOcrWord*, UINT32*, int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(UINT32, UINT32, IOcrWord**, UINT32*) = 0;
};

struct ILanguageVec : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32, ILanguage**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE IndexOf(ILanguage*, UINT32*, int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(UINT32, UINT32, ILanguage**, UINT32*) = 0;
};

// IReference<double> — get_Value at vtable[6] --------------------------------
struct IRefDouble : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Value(double*) = 0;
};

// ---------------------------------------------------------------------------

namespace remote_hands::platform {

namespace {

// Interface GUIDs used for QueryInterface calls
static const GUID IID_IAsyncInfo_RH = {
    0x00000036, 0x0000, 0x0000, {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID IID_IMemoryBuffer_RH = {
    0xfbc4dd2a, 0x245b, 0x11e4, {0xaf,0x98,0x68,0x94,0x23,0x26,0x0c,0xf8}};

// RoGetActivationFactory IIDs
static const GUID IID_IOcrEngineStatics_RH = {
    0x5bffa85a, 0x3384, 0x3540, {0x99,0x40,0x69,0x91,0x20,0xd4,0x28,0xa8}};
static const GUID IID_ISoftwareBitmapFactory_RH = {
    0xc99feb69, 0x2d62, 0x4d47, {0xa6,0xb3,0x4f,0xdb,0x6a,0x07,0xfd,0xf8}};

// ---------------------------------------------------------------------------
// WinRT runtime functions (loaded via GetProcAddress from combase.dll)

using PFN_RoInitialize             = HRESULT(WINAPI*)(int flags);
using PFN_RoGetActivationFactory   = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
using PFN_WindowsCreateString      = HRESULT(WINAPI*)(LPCWSTR, UINT32, HSTRING*);
using PFN_WindowsDeleteString      = HRESULT(WINAPI*)(HSTRING);
using PFN_WindowsGetStringRawBuffer = LPCWSTR(WINAPI*)(HSTRING, UINT32*);

struct WinRTFuncs {
    PFN_RoInitialize             RoInitialize             = nullptr;
    PFN_RoGetActivationFactory   RoGetActivationFactory   = nullptr;
    PFN_WindowsCreateString      WindowsCreateString      = nullptr;
    PFN_WindowsDeleteString      WindowsDeleteString      = nullptr;
    PFN_WindowsGetStringRawBuffer WindowsGetStringRawBuffer = nullptr;
    bool ready = false;
};

static HMODULE     s_combase = nullptr;
static WinRTFuncs  g_winrt;
static OcrCapabilities g_capabilities;

// GDI+ init for decode_bytes_to_frame (separate token from image_encode.cpp)
static std::once_flag s_gdip_once_ocr;
static ULONG_PTR      s_gdip_token_ocr = 0;

// ---------------------------------------------------------------------------
// Helpers

std::string narrow_utf8(const wchar_t* ws, UINT32 len) {
    if (!ws || len == 0) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, ws, static_cast<int>(len),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, static_cast<int>(len),
                        out.data(), n, nullptr, nullptr);
    return out;
}

// Consumes hs (calls WindowsDeleteString).
std::string hstring_to_utf8(HSTRING hs) {
    if (!hs) return {};
    UINT32 len = 0;
    const wchar_t* ws = g_winrt.WindowsGetStringRawBuffer(hs, &len);
    std::string s = narrow_utf8(ws, len);
    g_winrt.WindowsDeleteString(hs);
    return s;
}

// Caller owns the returned HSTRING (must call WindowsDeleteString).
HSTRING make_hstring(const wchar_t* ws) {
    HSTRING hs = nullptr;
    const std::size_t len = wcslen(ws);
    g_winrt.WindowsCreateString(ws, static_cast<UINT32>(len), &hs);
    return hs;
}

// Poll IAsyncInfo::get_Status until Completed or error (1 ms sleep interval).
HRESULT await_op(IUnknown* op) {
    IAsyncInfoShim* info = nullptr;
    HRESULT hr = op->QueryInterface(IID_IAsyncInfo_RH,
                                    reinterpret_cast<void**>(&info));
    if (FAILED(hr) || !info) return E_NOINTERFACE;

    for (;;) {
        AsyncStatus status = AsyncStatus::Started;
        info->get_Status(&status);
        if (status != AsyncStatus::Started) {
            HRESULT err = S_OK;
            if (status == AsyncStatus::Error) info->get_ErrorCode(&err);
            info->Close();
            info->Release();
            if (status == AsyncStatus::Completed) return S_OK;
            return FAILED(err) ? err : E_FAIL;
        }
        Sleep(1);
    }
}

// IAsyncOperation<T*>::GetResults is always at vtable[8] regardless of T.
HRESULT get_async_result(IUnknown* op, void** out) {
    return reinterpret_cast<IAsyncOpResult*>(op)->GetResults(out);
}

void ensure_gdiplus_ocr() {
    std::call_once(s_gdip_once_ocr, [] {
        Gdiplus::GdiplusStartupInput input{};
        Gdiplus::GdiplusStartup(&s_gdip_token_ocr, &input, nullptr);
    });
}

bool load_winrt() {
    s_combase = LoadLibraryW(L"combase.dll");
    if (!s_combase) return false;

    g_winrt.RoInitialize = reinterpret_cast<PFN_RoInitialize>(
        GetProcAddress(s_combase, "RoInitialize"));
    g_winrt.RoGetActivationFactory = reinterpret_cast<PFN_RoGetActivationFactory>(
        GetProcAddress(s_combase, "RoGetActivationFactory"));
    g_winrt.WindowsCreateString = reinterpret_cast<PFN_WindowsCreateString>(
        GetProcAddress(s_combase, "WindowsCreateString"));
    g_winrt.WindowsDeleteString = reinterpret_cast<PFN_WindowsDeleteString>(
        GetProcAddress(s_combase, "WindowsDeleteString"));
    g_winrt.WindowsGetStringRawBuffer = reinterpret_cast<PFN_WindowsGetStringRawBuffer>(
        GetProcAddress(s_combase, "WindowsGetStringRawBuffer"));

    g_winrt.ready = g_winrt.RoInitialize
                 && g_winrt.RoGetActivationFactory
                 && g_winrt.WindowsCreateString
                 && g_winrt.WindowsDeleteString
                 && g_winrt.WindowsGetStringRawBuffer;
    // s_combase is intentionally not FreeLibrary'd — it must stay mapped for
    // the lifetime of all COM vtable calls dispatched through WinRT objects.
    return g_winrt.ready;
}

IOcrEngineStatics* get_ocr_statics() {
    HSTRING hs = make_hstring(L"Windows.Media.Ocr.OcrEngine");
    if (!hs) return nullptr;
    void* statics = nullptr;
    g_winrt.RoGetActivationFactory(hs, IID_IOcrEngineStatics_RH, &statics);
    g_winrt.WindowsDeleteString(hs);
    return reinterpret_cast<IOcrEngineStatics*>(statics);
}

ISoftwareBitmapFactory* get_bmp_factory() {
    HSTRING hs = make_hstring(L"Windows.Graphics.Imaging.SoftwareBitmap");
    if (!hs) return nullptr;
    void* factory = nullptr;
    g_winrt.RoGetActivationFactory(hs, IID_ISoftwareBitmapFactory_RH, &factory);
    g_winrt.WindowsDeleteString(hs);
    return reinterpret_cast<ISoftwareBitmapFactory*>(factory);
}

// Create an ISoftwareBitmap (Bgra8, Ignore alpha) and copy BGRA pixels into it.
// pixels layout: top-down BGRA8888, stride = width * 4 (matches CapturedFrame).
// Returns nullptr on failure; caller owns the returned pointer.
ISoftwareBitmap* make_software_bitmap(const std::byte* pixels,
                                      int width, int height) {
    ISoftwareBitmapFactory* factory = get_bmp_factory();
    if (!factory) return nullptr;

    ISoftwareBitmap* bmp = nullptr;
    // BitmapPixelFormat::Bgra8 = 87, BitmapAlphaMode::Ignore = 2
    const HRESULT create_hr = factory->CreateWithAlpha(
        87, static_cast<INT32>(width), static_cast<INT32>(height), 2, &bmp);
    factory->Release();
    if (FAILED(create_hr) || !bmp) return nullptr;

    // Lock for write; buf holds the pixel lock until its final Release().
    IBitmapBuffer* buf = nullptr;
    if (FAILED(bmp->LockBuffer(2 /* Write */, &buf)) || !buf) {
        bmp->Release();
        return nullptr;
    }

    // QI IBitmapBuffer for IMemoryBuffer; buf still holds the lock.
    IMemoryBufferShim* mem_buf = nullptr;
    buf->QueryInterface(IID_IMemoryBuffer_RH, reinterpret_cast<void**>(&mem_buf));
    if (!mem_buf) { buf->Release(); bmp->Release(); return nullptr; }

    // Get a memory reference from IMemoryBuffer.
    IUnknown* mem_ref_raw = nullptr;
    mem_buf->CreateReference(&mem_ref_raw);
    mem_buf->Release();  // release the IMemoryBuffer ref; buf still holds the lock
    if (!mem_ref_raw) { buf->Release(); bmp->Release(); return nullptr; }

    // QI the reference for IMemoryBufferByteAccess to get the raw pointer.
    IMemoryBufferByteAccess* byte_access = nullptr;
    mem_ref_raw->QueryInterface(__uuidof(IMemoryBufferByteAccess),
                                reinterpret_cast<void**>(&byte_access));
    mem_ref_raw->Release();
    if (!byte_access) { buf->Release(); bmp->Release(); return nullptr; }

    BYTE*  data     = nullptr;
    UINT32 capacity = 0;
    byte_access->GetBuffer(&data, &capacity);

    const std::size_t pixel_bytes =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    bool ok = (data != nullptr && capacity >= pixel_bytes);
    if (ok) std::memcpy(data, pixels, pixel_bytes);

    byte_access->Release();
    buf->Release();  // LAST: releases the pixel lock

    if (!ok) { bmp->Release(); return nullptr; }
    return bmp;
}

// Decode arbitrary image bytes to a BGRA CapturedFrame using GDI+.
// Throws on decode failure or unsupported format.
screen::CapturedFrame decode_bytes_to_frame(const std::uint8_t* data,
                                            std::size_t len) {
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
        throw std::runtime_error("engine_error");

    ULONG written = 0;
    const HRESULT wr = stream->Write(data, static_cast<ULONG>(len), &written);
    if (FAILED(wr) || written != static_cast<ULONG>(len)) {
        stream->Release();
        throw std::runtime_error("engine_error");
    }
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);

    ensure_gdiplus_ocr();

    Gdiplus::Image* img_raw = Gdiplus::Image::FromStream(stream);
    stream->Release();

    if (!img_raw || img_raw->GetLastStatus() != Gdiplus::Ok) {
        delete img_raw;
        throw std::runtime_error("unsupported_format");
    }
    std::unique_ptr<Gdiplus::Image> img(img_raw);

    const int w = static_cast<int>(img->GetWidth());
    const int h = static_cast<int>(img->GetHeight());
    if (w <= 0 || h <= 0) throw std::runtime_error("engine_error");

    Gdiplus::Bitmap bmp(w, h, PixelFormat32bppARGB);
    if (bmp.GetLastStatus() != Gdiplus::Ok) throw std::runtime_error("engine_error");
    {
        Gdiplus::Graphics g(&bmp);
        g.DrawImage(img.get(), 0, 0, w, h);
    }
    img.reset();

    Gdiplus::BitmapData bmd{};
    Gdiplus::Rect rect(0, 0, w, h);
    if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead,
                     PixelFormat32bppARGB, &bmd) != Gdiplus::Ok) {
        throw std::runtime_error("engine_error");
    }

    const int stride = bmd.Stride;
    if (stride <= 0) { bmp.UnlockBits(&bmd); throw std::runtime_error("engine_error"); }

    screen::CapturedFrame frame;
    frame.width  = w;
    frame.height = h;
    frame.pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);

    const auto* src = reinterpret_cast<const BYTE*>(bmd.Scan0);
    const std::size_t row_bytes = static_cast<std::size_t>(w) * 4;
    if (static_cast<std::size_t>(stride) == row_bytes) {
        std::memcpy(frame.pixels.data(), src, frame.pixels.size());
    } else {
        for (int row = 0; row < h; ++row) {
            std::memcpy(
                frame.pixels.data() + static_cast<std::size_t>(row) * row_bytes,
                src + static_cast<std::size_t>(row) * static_cast<std::size_t>(stride),
                row_bytes);
        }
    }

    bmp.UnlockBits(&bmd);
    return frame;
}

// Build the OcrRecognition result from an IOcrResult COM object.
OcrRecognition map_result(IOcrResult* result,
                          int img_w, int img_h,
                          int frame_x, int frame_y,
                          float /*min_confidence*/,
                          bool include_word_bboxes) {
    OcrRecognition rec;
    rec.image_width  = img_w;
    rec.image_height = img_h;

    // TextAngle — may be null if the engine detected no rotation.
    void* angle_raw = nullptr;
    result->get_TextAngle(&angle_raw);
    if (angle_raw) {
        double angle_val = 0.0;
        reinterpret_cast<IRefDouble*>(angle_raw)->get_Value(&angle_val);
        rec.text_angle = static_cast<float>(angle_val);
        reinterpret_cast<IUnknown*>(angle_raw)->Release();
    }

    void* lines_raw = nullptr;
    result->get_Lines(&lines_raw);
    if (!lines_raw) return rec;

    auto* lines = reinterpret_cast<IOcrLineVec*>(lines_raw);
    UINT32 line_count = 0;
    lines->get_Size(&line_count);

    for (UINT32 i = 0; i < line_count; ++i) {
        IOcrLine* line = nullptr;
        if (FAILED(lines->GetAt(i, &line)) || !line) continue;

        OcrLine out_line;

        HSTRING line_text_hs = nullptr;
        line->get_Text(&line_text_hs);
        out_line.text = hstring_to_utf8(line_text_hs);

        if (include_word_bboxes) {
            void* words_raw = nullptr;
            line->get_Words(&words_raw);
            if (words_raw) {
                auto* words = reinterpret_cast<IOcrWordVec*>(words_raw);
                UINT32 word_count = 0;
                words->get_Size(&word_count);

                int line_min_x = INT_MAX, line_min_y = INT_MAX;
                int line_max_x = INT_MIN, line_max_y = INT_MIN;

                for (UINT32 j = 0; j < word_count; ++j) {
                    IOcrWord* word = nullptr;
                    if (FAILED(words->GetAt(j, &word)) || !word) continue;

                    WinRTRect wr{};
                    word->get_BoundingRect(&wr);
                    HSTRING word_text_hs = nullptr;
                    word->get_Text(&word_text_hs);
                    word->Release();

                    OcrWord out_word;
                    out_word.text = hstring_to_utf8(word_text_hs);
                    out_word.x = frame_x + static_cast<int>(wr.X);
                    out_word.y = frame_y + static_cast<int>(wr.Y);
                    out_word.w = static_cast<int>(wr.Width);
                    out_word.h = static_cast<int>(wr.Height);

                    line_min_x = (std::min)(line_min_x, out_word.x);
                    line_min_y = (std::min)(line_min_y, out_word.y);
                    line_max_x = (std::max)(line_max_x, out_word.x + out_word.w);
                    line_max_y = (std::max)(line_max_y, out_word.y + out_word.h);

                    out_line.words.push_back(std::move(out_word));
                }
                reinterpret_cast<IUnknown*>(words_raw)->Release();

                if (!out_line.words.empty() && line_min_x != INT_MAX) {
                    out_line.x = line_min_x;  out_line.y = line_min_y;
                    out_line.w = line_max_x - line_min_x;
                    out_line.h = line_max_y - line_min_y;
                }
            }
        }

        line->Release();
        if (!out_line.text.empty() || !out_line.words.empty())
            rec.lines.push_back(std::move(out_line));
    }

    reinterpret_cast<IUnknown*>(lines_raw)->Release();
    return rec;
}

// Create an IOcrEngine for the given language hint.
// Falls back to TryCreateFromUserProfileLanguages if no match is found.
IOcrEngine* make_engine(const std::string& language_hint) {
    IOcrEngineStatics* statics = get_ocr_statics();
    if (!statics) return nullptr;

    IOcrEngine* engine = nullptr;

    if (!language_hint.empty()) {
        void* lang_vec_raw = nullptr;
        statics->get_AvailableRecognizerLanguages(&lang_vec_raw);
        if (lang_vec_raw) {
            auto* lang_vec = reinterpret_cast<ILanguageVec*>(lang_vec_raw);
            UINT32 count = 0;
            lang_vec->get_Size(&count);

            for (UINT32 i = 0; i < count && !engine; ++i) {
                ILanguage* lang = nullptr;
                if (FAILED(lang_vec->GetAt(i, &lang)) || !lang) continue;

                HSTRING tag_hs = nullptr;
                lang->get_LanguageTag(&tag_hs);
                UINT32 tag_len = 0;
                const wchar_t* tag_ws =
                    g_winrt.WindowsGetStringRawBuffer(tag_hs, &tag_len);

                bool match = (tag_ws != nullptr
                              && tag_len == static_cast<UINT32>(language_hint.size()));
                for (UINT32 j = 0; j < tag_len && match; ++j) {
                    if (::tolower(static_cast<int>(tag_ws[j])) !=
                        ::tolower(static_cast<unsigned char>(language_hint[j]))) {
                        match = false;
                    }
                }
                g_winrt.WindowsDeleteString(tag_hs);

                if (match) statics->TryCreateFromLanguage(lang, &engine);
                lang->Release();
            }
            reinterpret_cast<IUnknown*>(lang_vec_raw)->Release();
        }
    }

    if (!engine) statics->TryCreateFromUserProfileLanguages(&engine);
    statics->Release();
    return engine;
}

// Run OCR on an already-created ISoftwareBitmap.
OcrRecognition do_ocr(ISoftwareBitmap* bmp,
                      int img_w, int img_h,
                      int frame_x, int frame_y,
                      const std::string& language_hint,
                      float min_confidence,
                      bool include_word_bboxes) {
    IOcrEngine* engine = make_engine(language_hint);
    if (!engine) throw std::runtime_error("not_supported");

    // Capture the engine language before dispatching the async op.
    std::string lang_used;
    {
        void* lang_raw = nullptr;
        engine->get_RecognizerLanguage(&lang_raw);
        if (lang_raw) {
            HSTRING tag_hs = nullptr;
            reinterpret_cast<ILanguage*>(lang_raw)->get_LanguageTag(&tag_hs);
            lang_used = hstring_to_utf8(tag_hs);
            reinterpret_cast<IUnknown*>(lang_raw)->Release();
        }
    }

    void* async_op_raw = nullptr;
    const HRESULT hr = engine->RecognizeAsync(
        reinterpret_cast<IUnknown*>(bmp), &async_op_raw);
    engine->Release();
    if (FAILED(hr) || !async_op_raw) throw std::runtime_error("engine_error");

    IUnknown* async_op = reinterpret_cast<IUnknown*>(async_op_raw);

    const HRESULT wait_hr = await_op(async_op);
    if (FAILED(wait_hr)) {
        async_op->Release();
        throw std::runtime_error("engine_error");
    }

    void* result_raw = nullptr;
    if (FAILED(get_async_result(async_op, &result_raw)) || !result_raw) {
        async_op->Release();
        throw std::runtime_error("engine_error");
    }
    async_op->Release();

    auto* result = reinterpret_cast<IOcrResult*>(result_raw);
    OcrRecognition rec = map_result(result, img_w, img_h, frame_x, frame_y,
                                    min_confidence, include_word_bboxes);
    rec.language_used = std::move(lang_used);
    result->Release();
    return rec;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public platform seam functions

void init_ocr() {
    if (!IsWindows8Point1OrGreater()) return;
    if (!load_winrt()) return;

    // RO_INIT_SINGLETHREADED = 0 (STA) — matches main()'s COINIT_APARTMENTTHREADED.
    // 0x80010106 = RPC_E_CHANGED_MODE: COM already initialised with a different
    // mode (rare but acceptable — OCR may still work if mode is compatible).
    const HRESULT hr = g_winrt.RoInitialize(0);
    if (FAILED(hr) && hr != static_cast<HRESULT>(0x80010106)) return;

    IOcrEngineStatics* statics = get_ocr_statics();
    if (!statics) return;

    UINT32 max_dim = 2048;
    statics->get_MaxImageDimension(&max_dim);
    g_capabilities.max_dimension = static_cast<int>(max_dim);

    void* lang_vec_raw = nullptr;
    statics->get_AvailableRecognizerLanguages(&lang_vec_raw);
    if (lang_vec_raw) {
        auto* lang_vec = reinterpret_cast<ILanguageVec*>(lang_vec_raw);
        UINT32 count = 0;
        lang_vec->get_Size(&count);
        for (UINT32 i = 0; i < count; ++i) {
            ILanguage* lang = nullptr;
            if (FAILED(lang_vec->GetAt(i, &lang)) || !lang) continue;
            HSTRING tag_hs = nullptr;
            lang->get_LanguageTag(&tag_hs);
            std::string tag = hstring_to_utf8(tag_hs);
            if (!tag.empty()) g_capabilities.languages.push_back(std::move(tag));
            lang->Release();
        }
        reinterpret_cast<IUnknown*>(lang_vec_raw)->Release();
    }

    if (!g_capabilities.languages.empty())
        g_capabilities.formats = {"png", "jpeg", "bmp"};

    statics->Release();
}

const OcrCapabilities& ocr_capabilities() {
    return g_capabilities;
}

OcrRecognition ocr_from_frame(
    const screen::CapturedFrame& frame,
    int frame_x, int frame_y,
    const std::string& language_hint,
    float min_confidence,
    bool include_word_bboxes)
{
    if (g_capabilities.languages.empty()) throw std::runtime_error("not_supported");

    ISoftwareBitmap* bmp = make_software_bitmap(
        frame.pixels.data(), frame.width, frame.height);
    if (!bmp) throw std::runtime_error("engine_error");

    OcrRecognition rec;
    try {
        rec = do_ocr(bmp, frame.width, frame.height, frame_x, frame_y,
                     language_hint, min_confidence, include_word_bboxes);
    } catch (...) {
        bmp->Release();
        throw;
    }
    bmp->Release();
    return rec;
}

OcrRecognition ocr_from_bytes(
    const std::uint8_t* data,
    std::size_t len,
    const std::string& /*format*/,
    const std::string& language_hint,
    float min_confidence,
    bool include_word_bboxes)
{
    if (g_capabilities.languages.empty()) throw std::runtime_error("not_supported");

    // GDI+ auto-detects format from magic bytes; the format hint is unused.
    screen::CapturedFrame frame = decode_bytes_to_frame(data, len);

    ISoftwareBitmap* bmp = make_software_bitmap(
        frame.pixels.data(), frame.width, frame.height);
    if (!bmp) throw std::runtime_error("engine_error");

    OcrRecognition rec;
    try {
        rec = do_ocr(bmp, frame.width, frame.height, 0, 0,
                     language_hint, min_confidence, include_word_bboxes);
    } catch (...) {
        bmp->Release();
        throw;
    }
    bmp->Release();
    return rec;
}

}  // namespace remote_hands::platform

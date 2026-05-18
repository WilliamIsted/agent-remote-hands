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

// windows-legacy PNG encode seam.
// Uses GDI+ (gdiplus.dll, available since Windows XP) instead of WIC.
// GdiplusStartup is called lazily on first use via std::call_once.

#include "platform.hpp"

#include <algorithm>
#include <mutex>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>   // CreateStreamOnHGlobal, IStream

// GdiplusTypes.h uses bare min()/max(); with NOMINMAX the Win32 macros are
// suppressed, so promote std::min/max into global scope before the include.
using std::min;
using std::max;
#include <gdiplus.h>

namespace remote_hands::platform {

namespace {

std::once_flag s_gdip_once;
ULONG_PTR      s_gdip_token = 0;

void ensure_gdiplus() {
    std::call_once(s_gdip_once, [] {
        Gdiplus::GdiplusStartupInput input{};
        Gdiplus::GdiplusStartup(&s_gdip_token, &input, nullptr);
    });
}

CLSID get_png_encoder_clsid() {
    UINT count = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&count, &size);
    if (size == 0) return CLSID{};

    std::vector<BYTE> buf(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(count, size, codecs);

    for (UINT i = 0; i < count; ++i) {
        if (std::wstring(codecs[i].MimeType) == L"image/png") {
            return codecs[i].Clsid;
        }
    }
    return CLSID{};
}

}  // namespace

std::vector<std::byte> encode_png(const screen::CapturedFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.pixels.empty()) {
        return {};
    }

    ensure_gdiplus();

    // Wrap the BGRA pixel buffer in a GDI+ Bitmap.
    // PixelFormat32bppARGB stores pixels as [B,G,R,A] in memory — same layout
    // as CapturedFrame (BGRA, stride = width*4).
    const UINT stride = static_cast<UINT>(frame.width) * 4;
    Gdiplus::Bitmap bmp(
        static_cast<INT>(frame.width),
        static_cast<INT>(frame.height),
        static_cast<INT>(stride),
        PixelFormat32bppARGB,
        // GDI+ takes non-const; pixels are not modified.
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(frame.pixels.data())));

    if (bmp.GetLastStatus() != Gdiplus::Ok) return {};

    const CLSID png_clsid = get_png_encoder_clsid();

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return {};

    const Gdiplus::Status status = bmp.Save(stream, &png_clsid);
    if (status != Gdiplus::Ok) {
        stream->Release();
        return {};
    }

    // Seek back and read the encoded bytes.
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);

    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) {
        stream->Release();
        return {};
    }

    std::vector<std::byte> out(static_cast<std::size_t>(stat.cbSize.QuadPart));
    ULONG read_bytes = 0;
    if (FAILED(stream->Read(out.data(),
                            static_cast<ULONG>(out.size()),
                            &read_bytes))) {
        stream->Release();
        return {};
    }
    stream->Release();
    out.resize(read_bytes);
    return out;
}

}  // namespace remote_hands::platform

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

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincodec.h>
#include <objbase.h>   // CreateStreamOnHGlobal

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

// ---------------------------------------------------------------------------
// Clipboard DIB ↔ PNG helpers (CF_DIB / CF_DIBV5 transcode).
//
// These are independent of the screen.capture encoder path above — added to
// support clipboard.get/set's `image` format. WIC handles every variant of
// BITMAPINFOHEADER/BITMAPV5HEADER the clipboard might hand us (Win32 DIB
// memory layouts are notoriously fiddly to hand-parse correctly: bitfield
// masks, palette tables, top-down vs bottom-up rows, sub-32bpp formats…).
//
// Strategy: synthesise a BITMAPFILEHEADER in front of the clipboard's CF_DIB
// block (which is "BMP minus the 14-byte file header") and feed that to a
// WIC BMP decoder; on the return path, decode the PNG, convert to 32bpp
// BGRA, then strip WIC's BMP file header back off to produce a CF_DIB block.
// This keeps the parsing of DIB internals on WIC's side, not ours.

namespace clipboard_dib_detail {

// Compute the pixel-data offset within a CF_DIB block. CF_DIB starts with a
// BITMAPINFOHEADER (or BITMAPV5HEADER); palette/bitfield-mask data follows;
// then pixels. Mirrors the offset Win32 itself computes when GDI consumes a
// CF_DIB handle.
std::uint32_t compute_pixel_offset(const BITMAPINFOHEADER* bih) {
    std::uint32_t off = bih->biSize;
    // BI_BITFIELDS (3) and BI_ALPHABITFIELDS (6) carry 3 or 4 DWORD masks
    // immediately after the header for sub-32bpp colour space description.
    // **Only for 40-byte BITMAPINFOHEADER.** BITMAPV4HEADER (biSize=108) and
    // BITMAPV5HEADER (biSize=124) embed the masks inside the header itself
    // (bV4RedMask..bV4AlphaMask / bV5RedMask..bV5AlphaMask), so adding extra
    // mask bytes here would over-count by 12 or 16 and place the pixel
    // pointer past the actual pixel data. CF_DIBV5 images from modern apps
    // (typical screenshots pasted to the clipboard) hit this path with
    // biSize=124 + BI_BITFIELDS + 32bpp. Per R1 review.
    if (bih->biSize <= sizeof(BITMAPINFOHEADER)) {
        if (bih->biCompression == BI_BITFIELDS) {
            off += 3 * sizeof(DWORD);
        } else if (bih->biCompression == 6 /* BI_ALPHABITFIELDS */) {
            off += 4 * sizeof(DWORD);
        }
    }
    // Palette: present for <=8bpp images. biClrUsed=0 means "max for bpp".
    if (bih->biBitCount <= 8) {
        DWORD palette_entries = bih->biClrUsed;
        if (palette_entries == 0) {
            palette_entries = 1u << bih->biBitCount;
        }
        off += palette_entries * sizeof(RGBQUAD);
    }
    return off;
}

}  // namespace clipboard_dib_detail

bool dib_to_png(const void* dib_data, std::size_t dib_size,
                std::vector<std::byte>& out_png,
                int& width_out, int& height_out) {
    out_png.clear();
    width_out = 0;
    height_out = 0;
    if (dib_data == nullptr || dib_size < sizeof(BITMAPINFOHEADER)) {
        return false;
    }

    const auto* bih = static_cast<const BITMAPINFOHEADER*>(dib_data);
    if (bih->biSize < sizeof(BITMAPINFOHEADER) || bih->biSize > dib_size) {
        return false;
    }

    // Synthesise a BMP file with a 14-byte BITMAPFILEHEADER prefix so WIC's
    // BMP decoder can consume it. The pixel offset is computed from the
    // header — we don't need to know the actual pixel-data size; WIC walks
    // it via the stride/height fields.
    const std::uint32_t pixel_offset =
        clipboard_dib_detail::compute_pixel_offset(bih);
    const std::uint32_t total = static_cast<std::uint32_t>(
        14u + dib_size);

    std::vector<BYTE> bmp(total);
    bmp[0] = 'B';
    bmp[1] = 'M';
    // Little-endian writes (Win32 is LE on every supported arch).
    std::uint32_t v = total;
    std::memcpy(&bmp[2], &v, 4);
    std::uint16_t r = 0;
    std::memcpy(&bmp[6], &r, 2);
    std::memcpy(&bmp[8], &r, 2);
    v = 14u + pixel_offset;
    std::memcpy(&bmp[10], &v, 4);
    std::memcpy(&bmp[14], dib_data, dib_size);

    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory))) || factory == nullptr) {
        return false;
    }

    bool ok = false;
    IStream* in_stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    IStream* out_stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame_encoder = nullptr;
    IPropertyBag2* props = nullptr;

    do {
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &in_stream))) break;
        ULONG written = 0;
        if (FAILED(in_stream->Write(bmp.data(),
                                     static_cast<ULONG>(bmp.size()),
                                     &written)) ||
            written != bmp.size()) break;
        LARGE_INTEGER zero{};
        if (FAILED(in_stream->Seek(zero, STREAM_SEEK_SET, nullptr))) break;

        if (FAILED(factory->CreateDecoderFromStream(
                in_stream, nullptr, WICDecodeMetadataCacheOnDemand,
                &decoder))) break;
        if (FAILED(decoder->GetFrame(0, &frame))) break;

        UINT w = 0, h = 0;
        if (FAILED(frame->GetSize(&w, &h))) break;
        width_out = static_cast<int>(w);
        height_out = static_cast<int>(h);

        // Force 32bpp BGRA so the encoder's pixel-format negotiation has a
        // single known input regardless of the source DIB depth.
        if (FAILED(factory->CreateFormatConverter(&converter))) break;
        if (FAILED(converter->Initialize(
                frame, GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeCustom))) break;

        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &out_stream))) break;
        if (FAILED(factory->CreateEncoder(
                GUID_ContainerFormatPng, nullptr, &encoder))) break;
        if (FAILED(encoder->Initialize(out_stream,
                                        WICBitmapEncoderNoCache))) break;
        if (FAILED(encoder->CreateNewFrame(&frame_encoder, &props))) break;
        if (FAILED(frame_encoder->Initialize(props))) break;
        if (FAILED(frame_encoder->SetSize(w, h))) break;

        GUID format = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame_encoder->SetPixelFormat(&format))) break;
        if (FAILED(frame_encoder->WriteSource(converter, nullptr))) break;
        if (FAILED(frame_encoder->Commit())) break;
        if (FAILED(encoder->Commit())) break;

        LARGE_INTEGER zero2{};
        if (FAILED(out_stream->Seek(zero2, STREAM_SEEK_SET, nullptr))) break;
        STATSTG stat{};
        if (FAILED(out_stream->Stat(&stat, STATFLAG_NONAME))) break;
        out_png.resize(static_cast<std::size_t>(stat.cbSize.QuadPart));
        ULONG read = 0;
        if (FAILED(out_stream->Read(out_png.data(),
                                     static_cast<ULONG>(out_png.size()),
                                     &read))) break;
        out_png.resize(read);
        ok = true;
    } while (false);

    if (props)         props->Release();
    if (frame_encoder) frame_encoder->Release();
    if (encoder)       encoder->Release();
    if (out_stream)    out_stream->Release();
    if (converter)     converter->Release();
    if (frame)         frame->Release();
    if (decoder)       decoder->Release();
    if (in_stream)     in_stream->Release();
    factory->Release();
    if (!ok) out_png.clear();
    return ok;
}

bool png_to_dib(const std::byte* png_data, std::size_t png_size,
                std::vector<std::byte>& out_dib) {
    out_dib.clear();
    if (png_data == nullptr || png_size == 0) return false;

    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory))) || factory == nullptr) {
        return false;
    }

    bool ok = false;
    IStream* in_stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    IStream* out_stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame_encoder = nullptr;
    IPropertyBag2* props = nullptr;

    do {
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &in_stream))) break;
        ULONG written = 0;
        if (FAILED(in_stream->Write(png_data,
                                     static_cast<ULONG>(png_size),
                                     &written)) || written != png_size) break;
        LARGE_INTEGER zero{};
        if (FAILED(in_stream->Seek(zero, STREAM_SEEK_SET, nullptr))) break;

        if (FAILED(factory->CreateDecoderFromStream(
                in_stream, nullptr, WICDecodeMetadataCacheOnDemand,
                &decoder))) break;
        if (FAILED(decoder->GetFrame(0, &frame))) break;

        UINT w = 0, h = 0;
        if (FAILED(frame->GetSize(&w, &h))) break;
        if (w == 0 || h == 0) break;

        if (FAILED(factory->CreateFormatConverter(&converter))) break;
        if (FAILED(converter->Initialize(
                frame, GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeCustom))) break;

        // Encode through the BMP encoder, then strip the 14-byte
        // BITMAPFILEHEADER off so we're left with a CF_DIB-shaped block
        // (BITMAPINFOHEADER + pixels). Going via WIC's BMP encoder rather
        // than hand-rolling the DIB layout lets WIC pick a row stride that
        // matches what GDI expects (4-byte aligned).
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &out_stream))) break;
        if (FAILED(factory->CreateEncoder(
                GUID_ContainerFormatBmp, nullptr, &encoder))) break;
        if (FAILED(encoder->Initialize(out_stream,
                                        WICBitmapEncoderNoCache))) break;
        if (FAILED(encoder->CreateNewFrame(&frame_encoder, &props))) break;
        if (FAILED(frame_encoder->Initialize(props))) break;
        if (FAILED(frame_encoder->SetSize(w, h))) break;
        GUID format = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame_encoder->SetPixelFormat(&format))) break;
        if (FAILED(frame_encoder->WriteSource(converter, nullptr))) break;
        if (FAILED(frame_encoder->Commit())) break;
        if (FAILED(encoder->Commit())) break;

        LARGE_INTEGER zero2{};
        if (FAILED(out_stream->Seek(zero2, STREAM_SEEK_SET, nullptr))) break;
        STATSTG stat{};
        if (FAILED(out_stream->Stat(&stat, STATFLAG_NONAME))) break;
        const std::size_t bmp_size = static_cast<std::size_t>(stat.cbSize.QuadPart);
        if (bmp_size < 14u + sizeof(BITMAPINFOHEADER)) break;

        std::vector<BYTE> bmp(bmp_size);
        ULONG read = 0;
        if (FAILED(out_stream->Read(bmp.data(),
                                     static_cast<ULONG>(bmp.size()),
                                     &read))) break;
        if (read < 14u + sizeof(BITMAPINFOHEADER)) break;

        // Strip the BITMAPFILEHEADER (14 bytes) — CF_DIB starts at the
        // BITMAPINFOHEADER and contains everything that follows.
        const std::size_t dib_bytes = read - 14u;
        out_dib.resize(dib_bytes);
        std::memcpy(out_dib.data(), bmp.data() + 14, dib_bytes);
        ok = true;
    } while (false);

    if (props)         props->Release();
    if (frame_encoder) frame_encoder->Release();
    if (encoder)       encoder->Release();
    if (out_stream)    out_stream->Release();
    if (converter)     converter->Release();
    if (frame)         frame->Release();
    if (decoder)       decoder->Release();
    if (in_stream)     in_stream->Release();
    factory->Release();
    if (!ok) out_dib.clear();
    return ok;
}

}  // namespace remote_hands::image

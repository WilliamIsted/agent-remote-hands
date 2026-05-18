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

#include "platform.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace remote_hands::platform {

using Microsoft::WRL::ComPtr;

std::vector<std::byte> encode_png(const screen::CapturedFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.pixels.empty()) {
        return {};
    }

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory)))) {
        return {};
    }

    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return {};

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(
            GUID_ContainerFormatPng, nullptr, &encoder))) return {};
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return {};

    ComPtr<IWICBitmapFrameEncode> frame_encoder;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame_encoder, &props))) return {};
    if (FAILED(frame_encoder->Initialize(props.Get()))) return {};
    if (FAILED(frame_encoder->SetSize(
            static_cast<UINT>(frame.width),
            static_cast<UINT>(frame.height)))) return {};

    GUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame_encoder->SetPixelFormat(&format))) return {};

    const UINT stride = static_cast<UINT>(frame.width) * 4;
    if (FAILED(frame_encoder->WritePixels(
            static_cast<UINT>(frame.height), stride,
            static_cast<UINT>(frame.pixels.size()),
            const_cast<BYTE*>(reinterpret_cast<const BYTE*>(frame.pixels.data()))))) {
        return {};
    }
    if (FAILED(frame_encoder->Commit())) return {};
    if (FAILED(encoder->Commit())) return {};

    LARGE_INTEGER zero{};
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) return {};

    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return {};

    std::vector<std::byte> out(static_cast<std::size_t>(stat.cbSize.QuadPart));
    ULONG read = 0;
    if (FAILED(stream->Read(out.data(),
                            static_cast<ULONG>(out.size()), &read))) {
        return {};
    }
    out.resize(read);
    return out;
}

}  // namespace remote_hands::platform

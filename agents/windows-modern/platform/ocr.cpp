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

// windows-modern vision.ocr platform seam.
// Uses Windows.Media.Ocr (WinRT). Requires Windows 10 1803+.
//
// WinRT API notes (SDK 10.0.26100):
//   OcrResult   — TextAngle (IReference<Double>), Lines (IReadOnlyList<OcrLine>)
//   OcrLine     — Text (String), Words (IReadOnlyList<OcrWord>); NO BoundingRect
//   OcrWord     — Text (String), BoundingRect (Rect)
//   OcrEngine   — RecognizerLanguage, RecognizeAsync, TryCreate*, etc.
// Line bboxes are therefore derived by union of their word BoundingRects.

#include "platform.hpp"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace wg  = winrt::Windows::Globalization;
namespace wgi = winrt::Windows::Graphics::Imaging;
namespace wmo = winrt::Windows::Media::Ocr;
namespace wss = winrt::Windows::Storage::Streams;

namespace remote_hands::platform {

namespace {

OcrCapabilities g_capabilities{};

// Initialize WinRT apartment on calling thread; ignore RPC_E_CHANGED_MODE
// (apartment already initialized with a compatible type on this thread).
void init_apartment_safe() {
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    } catch (winrt::hresult_error const& e) {
        // 0x80010106 = RPC_E_CHANGED_MODE: already initialized, safe to continue.
        if (static_cast<uint32_t>(e.code()) != 0x80010106u) throw;
    }
}

wmo::OcrEngine make_engine(const std::string& language_hint) {
    if (!language_hint.empty()) {
        try {
            wg::Language lang(winrt::to_hstring(language_hint));
            auto engine = wmo::OcrEngine::TryCreateFromLanguage(lang);
            if (engine) return engine;
        } catch (...) {}
    }
    auto engine = wmo::OcrEngine::TryCreateFromUserProfileLanguages();
    if (!engine) throw std::runtime_error("not_supported");
    return engine;
}

// Map a recognized OcrResult to our OcrRecognition struct.
// Language is sourced from the engine (OcrResult has no RecognizedLanguage in
// SDK 26100). Line bboxes are derived from word BoundingRects (OcrLine has
// no BoundingRect; only OcrWord does).
OcrRecognition map_result(
    const wmo::OcrEngine& engine,
    const wmo::OcrResult& result,
    int offset_x, int offset_y,
    float /*min_confidence*/,   // Windows.Media.Ocr reports no confidence; treat all as 1.0
    bool include_word_bboxes)
{
    OcrRecognition out;

    auto angle_ref = result.TextAngle();
    if (angle_ref) out.text_angle = static_cast<float>(angle_ref.Value());

    out.language_used = winrt::to_string(engine.RecognizerLanguage().LanguageTag());

    for (auto const& line : result.Lines()) {
        OcrLine l;
        l.text = winrt::to_string(line.Text());
        l.has_confidence = false;

        // Derive line bbox from word bboxes (OcrLine has no BoundingRect).
        float min_x =  1e9f, min_y =  1e9f;
        float max_x = -1e9f, max_y = -1e9f;
        bool  has_words = false;

        for (auto const& word : line.Words()) {
            auto r = word.BoundingRect();
            min_x = std::min(min_x, r.X);
            min_y = std::min(min_y, r.Y);
            max_x = std::max(max_x, r.X + r.Width);
            max_y = std::max(max_y, r.Y + r.Height);
            has_words = true;

            if (include_word_bboxes) {
                OcrWord w;
                w.text = winrt::to_string(word.Text());
                w.x = static_cast<int>(r.X)      + offset_x;
                w.y = static_cast<int>(r.Y)      + offset_y;
                w.w = static_cast<int>(r.Width);
                w.h = static_cast<int>(r.Height);
                w.confidence = 0.0f;
                l.words.push_back(std::move(w));
            }
        }

        if (has_words) {
            l.x = static_cast<int>(min_x) + offset_x;
            l.y = static_cast<int>(min_y) + offset_y;
            l.w = static_cast<int>(max_x - min_x);
            l.h = static_cast<int>(max_y - min_y);
        } else {
            l.x = offset_x; l.y = offset_y; l.w = 0; l.h = 0;
        }

        out.lines.push_back(std::move(l));
    }

    return out;
}

// Create a SoftwareBitmap from a CapturedFrame's BGRA8888 pixels using a
// BitmapEncoder round-trip (avoids IMemoryBufferByteAccess header issues).
wgi::SoftwareBitmap frame_to_software_bitmap(const screen::CapturedFrame& frame) {
    wss::InMemoryRandomAccessStream stream;
    {
        auto encoder = wgi::BitmapEncoder::CreateAsync(
            wgi::BitmapEncoder::BmpEncoderId(), stream).get();
        const auto* px = reinterpret_cast<const uint8_t*>(frame.pixels.data());
        encoder.SetPixelData(
            wgi::BitmapPixelFormat::Bgra8,
            wgi::BitmapAlphaMode::Ignore,
            static_cast<uint32_t>(frame.width),
            static_cast<uint32_t>(frame.height),
            96.0, 96.0,
            { px, px + frame.pixels.size() });
        encoder.FlushAsync().get();
    }
    stream.Seek(0);
    auto decoder = wgi::BitmapDecoder::CreateAsync(stream).get();
    return decoder.GetSoftwareBitmapAsync().get();
}

}  // namespace

void init_ocr() {
    init_apartment_safe();
    g_capabilities.formats = {"png", "jpeg", "bmp"};
    g_capabilities.max_dimension = 2048;

    auto engine = wmo::OcrEngine::TryCreateFromUserProfileLanguages();
    if (!engine) return;  // no pack installed; languages stays empty

    g_capabilities.max_dimension =
        static_cast<int>(wmo::OcrEngine::MaxImageDimension());

    for (auto const& lang : wmo::OcrEngine::AvailableRecognizerLanguages()) {
        g_capabilities.languages.push_back(winrt::to_string(lang.LanguageTag()));
    }
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
    init_apartment_safe();

    if (frame.width == 0 || frame.height == 0 || frame.pixels.empty())
        throw std::runtime_error("invalid_args");

    const int max_dim = g_capabilities.max_dimension;
    if (frame.width > max_dim || frame.height > max_dim)
        throw std::runtime_error("image_too_large");

    auto engine = make_engine(language_hint);
    auto bmp    = frame_to_software_bitmap(frame);
    auto result = engine.RecognizeAsync(bmp).get();

    OcrRecognition out = map_result(engine, result, frame_x, frame_y,
                                    min_confidence, include_word_bboxes);
    out.image_width  = frame.width;
    out.image_height = frame.height;
    return out;
}

OcrRecognition ocr_from_bytes(
    const std::uint8_t* data,
    std::size_t len,
    const std::string& /*format*/,
    const std::string& language_hint,
    float min_confidence,
    bool include_word_bboxes)
{
    init_apartment_safe();

    // Write bytes into an in-memory stream; codec sniffed from magic bytes.
    wss::InMemoryRandomAccessStream stream;
    {
        wss::DataWriter writer(stream);
        writer.WriteBytes(winrt::array_view<const uint8_t>(data, data + len));
        writer.StoreAsync().get();
        writer.DetachStream();
    }
    stream.Seek(0);

    wgi::BitmapDecoder decoder{nullptr};
    try {
        decoder = wgi::BitmapDecoder::CreateAsync(stream).get();
    } catch (winrt::hresult_error const&) {
        throw std::runtime_error("unsupported_format");
    }

    auto soft_bmp = decoder.GetSoftwareBitmapAsync().get();
    if (soft_bmp.BitmapPixelFormat() != wgi::BitmapPixelFormat::Bgra8) {
        soft_bmp = wgi::SoftwareBitmap::Convert(
            soft_bmp, wgi::BitmapPixelFormat::Bgra8);
    }

    const int max_dim = g_capabilities.max_dimension;
    if (static_cast<int>(soft_bmp.PixelWidth())  > max_dim ||
        static_cast<int>(soft_bmp.PixelHeight()) > max_dim) {
        throw std::runtime_error("image_too_large");
    }

    auto engine = make_engine(language_hint);
    auto result = engine.RecognizeAsync(soft_bmp).get();

    OcrRecognition out = map_result(engine, result, 0, 0,
                                    min_confidence, include_word_bboxes);
    out.image_width  = static_cast<int>(soft_bmp.PixelWidth());
    out.image_height = static_cast<int>(soft_bmp.PixelHeight());
    return out;
}

}  // namespace remote_hands::platform

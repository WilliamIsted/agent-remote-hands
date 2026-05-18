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

// Platform seam — the only interface between shared/ and per-family platform/.
//
// Each agent family (windows-modern, windows-legacy) provides one .cpp per
// function below. Shared code calls through this interface; no #ifdef ladders
// live in shared/. (Decision D3 from the rebuild plan.)
//
// The 5 seams correspond to Win32 APIs that differ across the OS-version range:
//   rng        — BCryptGenRandom (Vista+) vs CryptGenRandom (XP+)
//   integrity  — TokenIntegrityLevel InfoClass (Vista+) vs XP no-op
//   encode_png — WIC/windowscodecs (Vista+) vs GDI+ (XP+)
//   inet_pton  — inet_pton (Vista+) vs manual IPv4 parse (XP+)
//   known_folder — SHGetKnownFolderPath (Vista+) vs SHGetFolderPath+CSIDL (XP+)

#include "screen_capture.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>   // in_addr

namespace remote_hands::platform {

// rng — fill a buffer with cryptographically random bytes.
// Modern: BCryptGenRandom(BCRYPT_USE_SYSTEM_PREFERRED_RNG)
// Legacy: CryptGenRandom via PROV_RSA_FULL
std::vector<uint8_t> generate_random_bytes(std::size_t n);

// integrity — decode the mandatory integrity level of a process token.
// Returns one of "untrusted"/"low"/"medium"/"high"/"system", or empty on
// failure.
// Modern: GetTokenInformation(TokenIntegrityLevel) + RID decode.
// Legacy: same on Vista+; returns "medium" on XP (TokenIntegrityLevel absent).
std::string get_integrity_level(HANDLE token);

// encode_png — encode a CapturedFrame as a PNG byte stream.
// Returns an empty vector on failure.
// Modern: Windows Imaging Component (WIC/windowscodecs.dll).
// Legacy: GDI+ (gdiplus.dll).
std::vector<std::byte> encode_png(const screen::CapturedFrame& frame);

// inet_pton_ipv4 — parse a dotted-decimal IPv4 string into in_addr.
// Returns true on success. Used by mdns.cpp to set up the multicast group.
// Modern: inet_pton(AF_INET, ...).
// Legacy: inet_addr() (IPv4 only; sufficient for the fixed mDNS address).
bool inet_pton_ipv4(const char* addr_str, in_addr* out);

// get_program_data_path — return the per-machine application-data root.
// Returns e.g. L"C:\\ProgramData" on Vista+ or the CSIDL_COMMON_APPDATA
// equivalent on XP.
// Modern: SHGetKnownFolderPath(FOLDERID_ProgramData).
// Legacy: SHGetFolderPath(CSIDL_COMMON_APPDATA).
std::wstring get_program_data_path();

// vision.ocr seam (modern: Windows.Media.Ocr; legacy: gated >= Win8.1)

struct OcrWord {
    std::string text;
    int x = 0, y = 0, w = 0, h = 0;
    float confidence = 0.0f;
};

struct OcrLine {
    std::string text;
    int x = 0, y = 0, w = 0, h = 0;
    bool has_confidence = false;
    float confidence = 0.0f;
    std::vector<OcrWord> words;   // populated only when include_word_bboxes=true
};

struct OcrRecognition {
    std::vector<OcrLine> lines;
    std::string language_used;   // BCP-47
    float text_angle = 0.0f;     // degrees clockwise
    int image_width  = 0;        // width of the buffer OCR ran on
    int image_height = 0;        // height of the buffer OCR ran on
};

struct OcrCapabilities {
    std::vector<std::string> languages;    // BCP-47 list; empty = no packs
    int max_dimension = 2048;
    std::vector<std::string> formats;      // e.g. ["png","jpeg","bmp"]
};

// Called once from main() before accepting connections.
void init_ocr();

// Returns the cached OcrCapabilities populated by init_ocr().
const OcrCapabilities& ocr_capabilities();

// OCR from a captured frame (region/window/monitor live source).
// frame_x, frame_y: virtual-screen origin of the captured region.
// Throws std::runtime_error on engine failure.
OcrRecognition ocr_from_frame(
    const screen::CapturedFrame& frame,
    int frame_x, int frame_y,
    const std::string& language_hint,
    float min_confidence,
    bool include_word_bboxes);

// OCR from raw image bytes (path or bytes wire source).
// Throws std::runtime_error on decode or engine failure.
OcrRecognition ocr_from_bytes(
    const std::uint8_t* data,
    std::size_t len,
    const std::string& format,
    const std::string& language_hint,
    float min_confidence,
    bool include_word_bboxes);

}  // namespace remote_hands::platform

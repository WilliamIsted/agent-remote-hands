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

// UTF-8 / wide-character conversion helpers.
//
// Header-only inline helpers used across verb namespaces. The wire is UTF-8
// throughout; Windows APIs we call are wide-character. These wrap
// MultiByteToWideChar / WideCharToMultiByte with a friendlier interface.

#include <string>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace remote_hands::text {

inline std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    const int wlen = MultiByteToWideChar(
        CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (wlen <= 0) return {};
    std::wstring out(static_cast<std::size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), wlen);
    return out;
}

inline std::string wide_to_utf8(const wchar_t* w, std::size_t len) {
    if (len == 0) return {};
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, w, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

inline std::string wide_to_utf8(const std::wstring& w) {
    return wide_to_utf8(w.c_str(), w.size());
}

inline std::string wide_to_utf8(std::wstring_view w) {
    return wide_to_utf8(w.data(), w.size());
}

}  // namespace remote_hands::text

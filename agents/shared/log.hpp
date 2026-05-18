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

// Lightweight wide-string logging.
// Header-only. Writes to stderr and OutputDebugStringW (visible in DebugView).
//
// Format strings use the standard wprintf grammar.
//
//   remote_hands::log::info(L"port=%u tier=%s", port, tier);

#include <cstdarg>
#include <cstdio>
#include <iterator>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace remote_hands::log {

enum class Level { Debug, Info, Warning, Error };

namespace detail {

inline const wchar_t* level_tag(Level lvl) noexcept {
    switch (lvl) {
        case Level::Debug:   return L"DEBUG";
        case Level::Info:    return L"INFO ";
        case Level::Warning: return L"WARN ";
        case Level::Error:   return L"ERROR";
    }
    return L"?    ";
}

inline void emit_v(Level lvl, const wchar_t* fmt, va_list args) noexcept {
    wchar_t buf[1024];
    int prefix = std::swprintf(buf, 16, L"[%s] ", level_tag(lvl));
    if (prefix < 0 || prefix >= 16) {
        prefix = 0;
    }
    std::vswprintf(buf + prefix, std::size(buf) - static_cast<std::size_t>(prefix), fmt, args);

    std::fputws(buf, stderr);
    std::fputwc(L'\n', stderr);

    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
}

}  // namespace detail

inline void debug(const wchar_t* fmt, ...) noexcept {
    va_list args; va_start(args, fmt);
    detail::emit_v(Level::Debug, fmt, args);
    va_end(args);
}

inline void info(const wchar_t* fmt, ...) noexcept {
    va_list args; va_start(args, fmt);
    detail::emit_v(Level::Info, fmt, args);
    va_end(args);
}

inline void warning(const wchar_t* fmt, ...) noexcept {
    va_list args; va_start(args, fmt);
    detail::emit_v(Level::Warning, fmt, args);
    va_end(args);
}

inline void error(const wchar_t* fmt, ...) noexcept {
    va_list args; va_start(args, fmt);
    detail::emit_v(Level::Error, fmt, args);
    va_end(args);
}

}  // namespace remote_hands::log

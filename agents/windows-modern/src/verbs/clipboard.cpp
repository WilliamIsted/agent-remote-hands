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

// `clipboard.*` namespace verb handlers.
//
// Implements PROTOCOL.md §4.9:
//   clipboard.read   (observe)
//   clipboard.write  (drive)  -- length-prefixed UTF-8 payload
//
// Plain-text only via CF_UNICODETEXT; richer formats (HTML, image, files)
// are not part of v2.

#include "../connection.hpp"
#include "../errors.hpp"
#include "../json.hpp"
#include "../log.hpp"

#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace remote_hands::clipboard_verbs {

namespace {

class ClipboardLock {
public:
    explicit ClipboardLock(HWND owner = nullptr) {
        // Retry briefly: clipboard lock contention is common but usually
        // releases within a few milliseconds.
        for (int i = 0; i < 5; ++i) {
            if (OpenClipboard(owner)) { ok_ = true; return; }
            Sleep(10);
        }
    }
    ~ClipboardLock() { if (ok_) CloseClipboard(); }
    explicit operator bool() const noexcept { return ok_; }
    ClipboardLock(const ClipboardLock&)            = delete;
    ClipboardLock& operator=(const ClipboardLock&) = delete;
private:
    bool ok_ = false;
};

}  // namespace

// ---------------------------------------------------------------------------
// clipboard.read

void read(Connection& conn, const wire::Request&) {
    ClipboardLock lock;
    if (!lock) {
        conn.writer().write_err(ErrorCode::LockHeld,
                                "{\"lock_type\":\"clipboard\"}");
        return;
    }

    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) {
        // No text on clipboard is not an error: empty payload.
        conn.writer().write_ok();
        return;
    }

    const auto* w = static_cast<const wchar_t*>(GlobalLock(handle));
    if (!w) {
        conn.writer().write_ok();
        return;
    }

    const int wlen = static_cast<int>(std::wcslen(w));
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, w, wlen, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(needed > 0 ? needed : 0), '\0');
    if (needed > 0) {
        WideCharToMultiByte(CP_UTF8, 0, w, wlen,
                            utf8.data(), needed, nullptr, nullptr);
    }

    GlobalUnlock(handle);
    conn.writer().write_ok(utf8);
}

// ---------------------------------------------------------------------------
// clipboard.write

void write(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"clipboard.write requires <length>\"}");
        return;
    }

    unsigned long long length = 0;
    {
        const auto* end = req.args[0].data() + req.args[0].size();
        const auto [p, ec] = std::from_chars(req.args[0].data(), end, length, 10);
        if (ec != std::errc{} || p != end) {
            conn.writer().write_err(
                ErrorCode::InvalidArgs,
                "{\"message\":\"length must be a non-negative integer\"}");
            return;
        }
    }

    auto payload = conn.reader().read_payload(static_cast<std::size_t>(length));

    // UTF-8 → UTF-16
    const int wlen = MultiByteToWideChar(
        CP_UTF8, 0,
        reinterpret_cast<const char*>(payload.data()),
        static_cast<int>(payload.size()),
        nullptr, 0);
    if (wlen < 0) {
        conn.writer().write_err(ErrorCode::InvalidArgs,
                                "{\"message\":\"payload is not valid UTF-8\"}");
        return;
    }

    HGLOBAL hglob = GlobalAlloc(GMEM_MOVEABLE,
                                static_cast<SIZE_T>((wlen + 1) * sizeof(wchar_t)));
    if (!hglob) {
        conn.writer().write_err(ErrorCode::NotSupported);
        return;
    }

    auto* wbuf = static_cast<wchar_t*>(GlobalLock(hglob));
    if (wlen > 0) {
        MultiByteToWideChar(CP_UTF8, 0,
                            reinterpret_cast<const char*>(payload.data()),
                            static_cast<int>(payload.size()),
                            wbuf, wlen);
    }
    wbuf[wlen] = L'\0';
    GlobalUnlock(hglob);

    ClipboardLock lock;
    if (!lock) {
        GlobalFree(hglob);
        conn.writer().write_err(ErrorCode::LockHeld,
                                "{\"lock_type\":\"clipboard\"}");
        return;
    }

    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, hglob)) {
        // Failure: we still own the global handle.
        GlobalFree(hglob);
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", GetLastError());
        conn.writer().write_err(ErrorCode::NotSupported, detail);
        return;
    }
    // SetClipboardData success transfers ownership of hglob to the OS.

    conn.writer().write_ok();
}

}  // namespace remote_hands::clipboard_verbs

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

// `watch.*` namespace verb handlers and concrete Subscription implementations.
//
// Implements PROTOCOL.md §4.10 / §6:
//   watch.region    — periodic capture, optionally --until-change.
//   watch.process   — auto-cancels on process exit.
//   watch.window    — appearance / disappearance of windows by title prefix.
//   watch.element   — auto-cancels when an element invalidates.
//   watch.file      — ReadDirectoryChangesW on the parent directory.
//   watch.registry  — RegNotifyChangeKeyValue on a key.
//   watch.cancel    — idempotent removal from the registry.
//
// Each watch's run loop polls a short timeout (~100 ms) so should_stop()
// gets a chance to break out of waits when the connection closes.
//
// Closes #49.

#include "../connection.hpp"
#include "../element_table.hpp"
#include "../errors.hpp"
#include "../image_encode.hpp"
#include "../json.hpp"
#include "../log.hpp"
#include "../screen_capture.hpp"
#include "../subscription.hpp"
#include "../text_util.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>          // Defines `interface` macro before UIA headers.
#include <UIAutomation.h>
#include <wrl/client.h>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

namespace remote_hands::watch_verbs {

using Microsoft::WRL::ComPtr;

namespace {

constexpr DWORD kPollMs = 100;

// ---------------------------------------------------------------------------
// Argument-parsing helpers

bool parse_uint(std::string_view s, unsigned long long& out) {
    const auto* end = s.data() + s.size();
    const auto [p, ec] = std::from_chars(s.data(), end, out, 10);
    return ec == std::errc{} && p == end;
}

bool parse_region4(std::string_view s, int& x, int& y, int& w, int& h) {
    int values[4] = {};
    std::size_t cursor = 0;
    for (int i = 0; i < 4; ++i) {
        const std::size_t comma = s.find(',', cursor);
        const std::size_t end_pos = (comma == std::string_view::npos) ? s.size() : comma;
        if (end_pos == cursor) return false;

        const auto* p_end = s.data() + end_pos;
        long parsed = 0;
        const auto [p, ec] = std::from_chars(s.data() + cursor, p_end, parsed, 10);
        if (ec != std::errc{} || p != p_end) return false;

        values[i] = static_cast<int>(parsed);
        if (comma == std::string_view::npos && i < 3) return false;
        cursor = end_pos + 1;
    }
    x = values[0]; y = values[1]; w = values[2]; h = values[3];
    return true;
}

// FNV-1a 64-bit hash over a byte buffer. Used by watch.region for
// "did the pixels change?" without keeping a full prior frame around.
std::uint64_t hash64(const std::byte* data, std::size_t len) {
    constexpr std::uint64_t prime  = 1099511628211ULL;
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    std::uint64_t h = offset;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[i]));
        h *= prime;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Concrete subscriptions

// watch.region — periodic capture; emits PNG-encoded frames.
class RegionWatch : public Subscription {
public:
    RegionWatch(wire::Writer& w, std::string id,
                int x, int y, int width, int height,
                int interval_ms, bool until_change)
        : Subscription(w, std::move(id)),
          x_{x}, y_{y}, w_{width}, h_{height},
          interval_ms_{interval_ms}, until_change_{until_change} {}

    // Join the worker before derived members are destroyed. ~Subscription()
    // also calls stop() as a safety net, but by then the most-derived members
    // are already gone — too late to prevent the worker reading freed data.
    // See the issue tracking the rc.7 watch-cancel destruction-order race.
    ~RegionWatch() override { stop(); }

protected:
    void run() override {
        std::uint64_t prev_hash = 0;
        bool first = true;

        while (!should_stop()) {
            auto frame = screen::capture_region(x_, y_, w_, h_);
            if (frame.pixels.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
                continue;
            }
            const std::uint64_t h = hash64(frame.pixels.data(), frame.pixels.size());

            if (until_change_) {
                if (!first && h != prev_hash) {
                    auto encoded = image::encode_png(frame);
                    if (!encoded.empty()) {
                        emit_bytes(std::span<const std::byte>(encoded));
                    }
                    return;  // auto-cancel
                }
                prev_hash = h;
                first = false;
            } else {
                auto encoded = image::encode_png(frame);
                if (!encoded.empty()) {
                    emit_bytes(std::span<const std::byte>(encoded));
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
        }
    }

private:
    int  x_, y_, w_, h_;
    int  interval_ms_;
    bool until_change_;
};

// watch.process — auto-cancels on process exit.
class ProcessWatch : public Subscription {
public:
    ProcessWatch(wire::Writer& w, std::string id, DWORD pid)
        : Subscription(w, std::move(id)), pid_{pid} {
        handle_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                              FALSE, pid);
    }

    ~ProcessWatch() override {
        // Join FIRST — the worker calls WaitForSingleObject(handle_) and
        // GetExitCodeProcess(handle_) every iteration, so closing the handle
        // before the thread is joined is a use-after-CloseHandle on a kernel
        // object whose slot may already have been reassigned.
        stop();
        if (handle_) CloseHandle(handle_);
    }

protected:
    void run() override {
        if (!handle_) {
            // Process already gone — emit and exit immediately.
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "{\"kind\":\"process_exit\",\"pid\":%lu}",
                          static_cast<unsigned long>(pid_));
            emit(buf);
            return;
        }
        while (!should_stop()) {
            const DWORD wr = WaitForSingleObject(handle_, kPollMs);
            if (wr == WAIT_OBJECT_0) {
                DWORD code = 0;
                GetExitCodeProcess(handle_, &code);
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                              "{\"kind\":\"process_exit\",\"pid\":%lu,\"exit_code\":%lu}",
                              static_cast<unsigned long>(pid_),
                              static_cast<unsigned long>(code));
                emit(buf);
                return;  // auto-cancel
            }
        }
    }

private:
    DWORD  pid_;
    HANDLE handle_ = nullptr;
};

// watch.window — diff windows matching a title prefix at 500 ms cadence.
class WindowWatch : public Subscription {
public:
    WindowWatch(wire::Writer& w, std::string id, std::string prefix)
        : Subscription(w, std::move(id)), prefix_{std::move(prefix)} {}

    // Join the worker before prefix_ is destructed — the run loop reads
    // prefix_ via EnumContext on every iteration, so destruction-while-running
    // is a UAF on the std::string's heap buffer. This was the rc.7 crash
    // (abort+0x35 = std::terminate from an exception escaping the worker).
    ~WindowWatch() override { stop(); }

protected:
    void run() override {
        std::unordered_set<HWND> seen;
        std::unordered_set<HWND> current;

        while (!should_stop()) {
            current.clear();
            EnumContext ctx{prefix_, current};
            EnumWindows(&WindowWatch::enum_proc, reinterpret_cast<LPARAM>(&ctx));

            for (HWND h : current) {
                if (seen.find(h) == seen.end()) {
                    emit_window_event(h, "window_appeared");
                }
            }
            for (HWND h : seen) {
                if (current.find(h) == current.end()) {
                    emit_window_event(h, "window_gone");
                }
            }
            seen.swap(current);

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

private:
    struct EnumContext {
        const std::string&         prefix;
        std::unordered_set<HWND>&  out;
    };

    static BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lparam) {
        auto* ctx = reinterpret_cast<EnumContext*>(lparam);
        if (!IsWindowVisible(hwnd)) return TRUE;
        wchar_t title[512];
        const int len = GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
        if (len <= 0) return TRUE;
        const std::string utf8 = text::wide_to_utf8(title, static_cast<std::size_t>(len));
        if (utf8.size() < ctx->prefix.size()) return TRUE;
        for (std::size_t i = 0; i < ctx->prefix.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(utf8[i])) !=
                std::tolower(static_cast<unsigned char>(ctx->prefix[i]))) {
                return TRUE;
            }
        }
        ctx->out.insert(hwnd);
        return TRUE;
    }

    void emit_window_event(HWND hwnd, std::string_view kind) {
        wchar_t title[512] = {};
        const int len = GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
        const std::string utf8_title =
            len > 0 ? text::wide_to_utf8(title, static_cast<std::size_t>(len))
                    : std::string{};

        char hbuf[32];
        std::snprintf(hbuf, sizeof(hbuf), "win:0x%llx",
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)));

        std::string body = "{";
        json::append_kv_string(body, "kind",  kind);       body += ',';
        json::append_kv_string(body, "hwnd",  hbuf);       body += ',';
        json::append_kv_string(body, "title", utf8_title);
        body += '}';
        emit(body);
    }

    std::string prefix_;
};

// watch.element — polls UIA_E_ELEMENTNOTAVAILABLE; auto-cancels on invalidation.
class ElementWatch : public Subscription {
public:
    ElementWatch(wire::Writer& w, std::string id, std::string element_handle,
                 IUIAutomationElement* elem)
        : Subscription(w, std::move(id)),
          element_handle_{std::move(element_handle)},
          elem_{elem} {
        if (elem_) elem_->AddRef();
    }

    ~ElementWatch() override {
        // Join FIRST. The worker holds a reference to elem_ and calls
        // get_CurrentBoundingRectangle on every iteration. Releasing the COM
        // ptr while the worker is still calling through it is a UAF on the
        // COM object — which on a busy system can mean reading another
        // thread's reused allocation.
        stop();
        if (elem_) elem_->Release();
    }

protected:
    void run() override {
        // COM apartment for this thread (each subscription thread is fresh).
        const HRESULT hr_init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        while (!should_stop()) {
            RECT rc{};
            const HRESULT hr = elem_->get_CurrentBoundingRectangle(&rc);
            if (hr == UIA_E_ELEMENTNOTAVAILABLE) {
                std::string body = "{";
                json::append_kv_string(body, "kind", "element_invalidated"); body += ',';
                json::append_kv_string(body, "elt", element_handle_);
                body += '}';
                emit(body);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        if (SUCCEEDED(hr_init)) CoUninitialize();
    }

private:
    std::string             element_handle_;
    IUIAutomationElement*   elem_ = nullptr;
};

// watch.file — ReadDirectoryChangesW on the parent dir of `pattern`,
// filtering on the filename component.
class FileWatch : public Subscription {
public:
    FileWatch(wire::Writer& w, std::string id, std::wstring pattern)
        : Subscription(w, std::move(id)), pattern_{std::move(pattern)} {}

    // Join the worker before pattern_ is destructed — the run loop derives
    // `dir` and `spec` from pattern_ at the top, but PathMatchSpecW reads
    // `spec` (which holds a pointer into the wstring's buffer) every event.
    ~FileWatch() override { stop(); }

protected:
    void run() override {
        std::wstring dir = pattern_;
        std::wstring spec = L"*";
        if (auto sep = pattern_.find_last_of(L"\\/"); sep != std::wstring::npos) {
            dir  = pattern_.substr(0, sep);
            spec = pattern_.substr(sep + 1);
            if (spec.empty()) spec = L"*";
        }

        HANDLE dir_handle = CreateFileW(
            dir.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (dir_handle == INVALID_HANDLE_VALUE) return;

        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) {
            CloseHandle(dir_handle);
            return;
        }

        constexpr DWORD kBufBytes = 16 * 1024;
        std::vector<BYTE> buffer(kBufBytes);

        while (!should_stop()) {
            OVERLAPPED ov{};
            ov.hEvent = event;
            ResetEvent(event);

            DWORD returned = 0;
            const BOOL ok = ReadDirectoryChangesW(
                dir_handle, buffer.data(), kBufBytes, FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME |
                FILE_NOTIFY_CHANGE_DIR_NAME  |
                FILE_NOTIFY_CHANGE_LAST_WRITE,
                &returned, &ov, nullptr);
            if (!ok) break;

            while (!should_stop()) {
                const DWORD wr = WaitForSingleObject(event, kPollMs);
                if (wr == WAIT_OBJECT_0) break;
                if (wr != WAIT_TIMEOUT) goto done;
            }
            if (should_stop()) {
                CancelIo(dir_handle);
                break;
            }

            DWORD bytes = 0;
            if (!GetOverlappedResult(dir_handle, &ov, &bytes, FALSE) || bytes == 0) {
                continue;
            }

            const auto* p = buffer.data();
            while (true) {
                const auto* info =
                    reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
                const std::wstring fname{info->FileName,
                                         info->FileNameLength / sizeof(wchar_t)};

                if (PathMatchSpecW(fname.c_str(), spec.c_str())) {
                    const char* kind =
                        (info->Action == FILE_ACTION_ADDED              ? "created"
                       : info->Action == FILE_ACTION_REMOVED            ? "deleted"
                       : info->Action == FILE_ACTION_MODIFIED           ? "modified"
                       : info->Action == FILE_ACTION_RENAMED_OLD_NAME   ? "renamed_from"
                       : info->Action == FILE_ACTION_RENAMED_NEW_NAME   ? "renamed_to"
                       : "changed");

                    std::wstring full = dir;
                    if (!full.empty() && full.back() != L'\\' && full.back() != L'/') {
                        full += L'\\';
                    }
                    full += fname;

                    std::string body = "{";
                    json::append_kv_string(body, "kind", kind); body += ',';
                    json::append_kv_string(body, "path", text::wide_to_utf8(full));
                    body += '}';
                    emit(body);
                }

                if (info->NextEntryOffset == 0) break;
                p += info->NextEntryOffset;
            }
        }
done:
        CloseHandle(event);
        CloseHandle(dir_handle);
    }

private:
    std::wstring pattern_;
};

// watch.registry — RegNotifyChangeKeyValue.
class RegistryWatch : public Subscription {
public:
    RegistryWatch(wire::Writer& w, std::string id, HKEY key, std::string path)
        : Subscription(w, std::move(id)), key_{key}, path_{std::move(path)} {}

    ~RegistryWatch() override {
        // Join FIRST. The worker calls RegNotifyChangeKeyValue(key_) on
        // every iteration; closing the registry handle before the thread is
        // joined is a use-after-RegCloseKey on a kernel object whose slot
        // may already have been reassigned to another caller's handle.
        stop();
        if (key_) RegCloseKey(key_);
    }

protected:
    void run() override {
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) return;

        while (!should_stop()) {
            ResetEvent(event);
            if (RegNotifyChangeKeyValue(
                    key_, TRUE,
                    REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
                    event, TRUE) != ERROR_SUCCESS) {
                break;
            }
            while (!should_stop()) {
                const DWORD wr = WaitForSingleObject(event, kPollMs);
                if (wr == WAIT_OBJECT_0) break;
                if (wr != WAIT_TIMEOUT) goto done;
            }
            if (should_stop()) break;

            std::string body = "{";
            json::append_kv_string(body, "kind", "changed"); body += ',';
            json::append_kv_string(body, "path", path_);
            body += '}';
            emit(body);
        }
done:
        CloseHandle(event);
    }

private:
    HKEY        key_  = nullptr;
    std::string path_;
};

// ---------------------------------------------------------------------------
// Common: register + return subscription_id

void register_and_ack(Connection& conn, std::unique_ptr<Subscription> sub) {
    sub->start();
    std::string body = "{";
    json::append_kv_string(body, "subscription_id", sub->id());
    body += '}';
    conn.subscriptions().register_subscription(std::move(sub));
    conn.writer().write_ok(body);
}

// Registry path helpers (cribbed from registry.cpp; will move to a shared
// header when a third caller appears).
HKEY parse_registry_root(std::string_view name) {
    if (name == "HKLM" || name == "HKEY_LOCAL_MACHINE")    return HKEY_LOCAL_MACHINE;
    if (name == "HKCU" || name == "HKEY_CURRENT_USER")     return HKEY_CURRENT_USER;
    if (name == "HKCR" || name == "HKEY_CLASSES_ROOT")     return HKEY_CLASSES_ROOT;
    if (name == "HKU"  || name == "HKEY_USERS")            return HKEY_USERS;
    if (name == "HKCC" || name == "HKEY_CURRENT_CONFIG")   return HKEY_CURRENT_CONFIG;
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// watch.region

void region(Connection& conn, const wire::Request& req) {
    if (req.args.empty()) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"watch.region requires <x,y,w,h> [--interval <ms>] [--until-change]\"}");
        return;
    }

    int x = 0, y = 0, w = 0, h = 0;
    if (!parse_region4(req.args[0], x, y, w, h)) {
        conn.writer().write_err(ErrorCode::InvalidArgs,
                                "{\"message\":\"first arg must be x,y,w,h\"}");
        return;
    }

    int  interval_ms = 500;
    bool until_change = false;
    for (std::size_t i = 1; i < req.args.size(); ++i) {
        if (req.args[i] == "--interval" && i + 1 < req.args.size()) {
            unsigned long long v = 0;
            if (!parse_uint(req.args[++i], v) || v == 0) {
                conn.writer().write_err(ErrorCode::InvalidArgs,
                                        "{\"message\":\"--interval must be a positive integer\"}");
                return;
            }
            interval_ms = static_cast<int>(v);
        } else if (req.args[i] == "--until-change") {
            until_change = true;
        } else if (req.args[i].size() >= 2 &&
                   req.args[i].compare(0, 2, "--") == 0) {
            std::string detail = "{\"unknown_flag\":\"";
            detail += req.args[i];
            detail += "\"}";
            conn.writer().write_err(ErrorCode::InvalidArgs, detail);
            return;
        }
    }

    auto sub = std::make_unique<RegionWatch>(
        conn.writer(), conn.subscriptions().allocate_id(),
        x, y, w, h, interval_ms, until_change);
    register_and_ack(conn, std::move(sub));
}

// ---------------------------------------------------------------------------
// watch.process

void process(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"watch.process requires <pid>\"}");
        return;
    }
    unsigned long long pid_v = 0;
    if (!parse_uint(req.args[0], pid_v)) {
        conn.writer().write_err(ErrorCode::InvalidArgs,
                                "{\"message\":\"pid must be a non-negative integer\"}");
        return;
    }

    auto sub = std::make_unique<ProcessWatch>(
        conn.writer(), conn.subscriptions().allocate_id(),
        static_cast<DWORD>(pid_v));
    register_and_ack(conn, std::move(sub));
}

// ---------------------------------------------------------------------------
// watch.window

void window(Connection& conn, const wire::Request& req) {
    std::string prefix;
    for (std::size_t i = 0; i < req.args.size(); ++i) {
        if (req.args[i] == "--title-prefix" && i + 1 < req.args.size()) {
            prefix = req.args[++i];
        } else if (req.args[i].size() >= 2 &&
                   req.args[i].compare(0, 2, "--") == 0) {
            std::string detail = "{\"unknown_flag\":\"";
            detail += req.args[i];
            detail += "\"}";
            conn.writer().write_err(ErrorCode::InvalidArgs, detail);
            return;
        }
    }
    if (prefix.empty()) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"watch.window requires --title-prefix <pattern>\"}");
        return;
    }

    auto sub = std::make_unique<WindowWatch>(
        conn.writer(), conn.subscriptions().allocate_id(), std::move(prefix));
    register_and_ack(conn, std::move(sub));
}

// ---------------------------------------------------------------------------
// watch.element

void element(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"watch.element requires <elt-id>\"}");
        return;
    }
    IUIAutomationElement* elem = conn.element_table().lookup(req.args[0]);
    if (!elem) {
        std::string detail = "{";
        json::append_kv_string(detail, "handle", req.args[0]);
        detail += '}';
        conn.writer().write_err(ErrorCode::TargetGone, detail);
        return;
    }

    auto sub = std::make_unique<ElementWatch>(
        conn.writer(), conn.subscriptions().allocate_id(),
        std::string{req.args[0]}, elem);
    register_and_ack(conn, std::move(sub));
}

// ---------------------------------------------------------------------------
// watch.file

void file(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"watch.file requires <pattern>\"}");
        return;
    }

    auto sub = std::make_unique<FileWatch>(
        conn.writer(), conn.subscriptions().allocate_id(),
        text::utf8_to_wide(req.args[0]));
    register_and_ack(conn, std::move(sub));
}

// ---------------------------------------------------------------------------
// watch.registry

void registry(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"watch.registry requires <path>\"}");
        return;
    }
    const auto& full = req.args[0];
    const auto sep = full.find('\\');
    if (sep == std::string::npos || sep == 0) {
        conn.writer().write_err(ErrorCode::InvalidArgs,
                                "{\"message\":\"unrecognised registry path\"}");
        return;
    }
    HKEY root = parse_registry_root(std::string_view{full}.substr(0, sep));
    if (!root) {
        conn.writer().write_err(ErrorCode::InvalidArgs,
                                "{\"message\":\"unrecognised registry root\"}");
        return;
    }

    const std::wstring sub_path = text::utf8_to_wide(full.substr(sep + 1));

    HKEY key = nullptr;
    if (RegOpenKeyExW(root, sub_path.c_str(), 0,
                      KEY_NOTIFY | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        conn.writer().write_err(ErrorCode::NotFound);
        return;
    }

    auto sub = std::make_unique<RegistryWatch>(
        conn.writer(), conn.subscriptions().allocate_id(), key, full);
    register_and_ack(conn, std::move(sub));
}

// ---------------------------------------------------------------------------
// watch.cancel

void cancel(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"watch.cancel requires <sub-id>\"}");
        return;
    }
    conn.subscriptions().cancel(req.args[0]);  // idempotent on unknown ids
    conn.writer().write_ok();
}

}  // namespace remote_hands::watch_verbs

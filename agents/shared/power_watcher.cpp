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

// power_watcher.cpp -- WM_POWERBROADCAST hidden-window + message-pump thread.
//
// Creates a message-only (HWND_MESSAGE) invisible window to receive
// WM_POWERBROADCAST messages. HWND_MESSAGE is available on Win2000+ and
// covers both the modern (Win10+) and legacy (XP SP3+) build targets.
//
// On PBT_APMSUSPEND : calls on_suspend (close listen socket cleanly).
// On any resume event: calls on_resume (open a new listen socket).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "power_watcher.hpp"
#include "log.hpp"

#include <atomic>
#include <functional>
#include <thread>

namespace remote_hands {

namespace {

// APM event codes — define fallbacks for SDK versions that omit them.
#ifndef PBT_APMSUSPEND
#define PBT_APMSUSPEND         0x0004
#endif
#ifndef PBT_APMRESUMESUSPEND
#define PBT_APMRESUMESUSPEND   0x0007
#endif
#ifndef PBT_APMRESUMESTANDBY
#define PBT_APMRESUMESTANDBY   0x0008
#endif
#ifndef PBT_APMRESUMECRITICAL
#define PBT_APMRESUMECRITICAL  0x0006
#endif
#ifndef PBT_APMRESUMEAUTOMATIC
#define PBT_APMRESUMEAUTOMATIC 0x0012
#endif

std::function<void()>  g_on_suspend;
std::function<void()>  g_on_resume;
std::atomic<DWORD>     g_pump_tid{0};

LRESULT CALLBACK power_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_POWERBROADCAST) {
        switch (wp) {
        case PBT_APMSUSPEND:
            log::debug(L"power: PBT_APMSUSPEND -- closing listen socket");
            if (g_on_suspend) g_on_suspend();
            break;
        case PBT_APMRESUMEAUTOMATIC:
        case PBT_APMRESUMESUSPEND:
        case PBT_APMRESUMESTANDBY:
        case PBT_APMRESUMECRITICAL:
            log::debug(L"power: resume event %u -- triggering socket rebind",
                       static_cast<unsigned>(wp));
            if (g_on_resume) g_on_resume();
            break;
        default:
            break;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void pump_thread_fn() {
    g_pump_tid.store(GetCurrentThreadId());

    wchar_t cls[64];
    // Unique class name per process to avoid conflicts if two agent instances
    // run simultaneously or a stale registration exists from a prior crash.
    _snwprintf_s(cls, _countof(cls), _TRUNCATE,
                 L"RhPowerWatcher_%lu",
                 static_cast<unsigned long>(GetCurrentProcessId()));

    WNDCLASSW wc{};
    wc.lpfnWndProc   = power_wndproc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;

    if (!RegisterClassW(&wc)) {
        log::warning(L"power: RegisterClassW failed (%lu)", GetLastError());
        return;
    }

    // Message-only window (HWND_MESSAGE): available on Win2000+ / XP SP3+.
    // WS_POPUP without WS_VISIBLE: invisible on creation.
    // WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE: never appears in Alt+Tab,
    // cannot steal focus.
    const HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        cls, L"",
        WS_POPUP,
        0, 0, 0, 0,
        HWND_MESSAGE,
        nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!hwnd) {
        log::warning(L"power: CreateWindowExW failed (%lu)", GetLastError());
        UnregisterClassW(cls, GetModuleHandleW(nullptr));
        return;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyWindow(hwnd);
    UnregisterClassW(cls, GetModuleHandleW(nullptr));
}

}  // namespace

void power_watcher_start(std::function<void()> on_suspend,
                         std::function<void()> on_resume) {
    g_on_suspend = std::move(on_suspend);
    g_on_resume  = std::move(on_resume);
    std::thread(pump_thread_fn).detach();
}

void power_watcher_stop() {
    const DWORD tid = g_pump_tid.load();
    if (tid) PostThreadMessageW(tid, WM_QUIT, 0, 0);
}

}  // namespace remote_hands

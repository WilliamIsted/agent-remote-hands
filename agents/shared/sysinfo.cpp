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

#include "sysinfo.hpp"

#include "platform.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cwchar>
#include <intrin.h>      // __cpuid (hypervisor-present bit)
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <lmcons.h>
#include <objbase.h>     // CoInitializeEx / CoCreateInstance (WMI)
#include <wbemidl.h>     // IWbemLocator / IWbemServices (Win32_ComputerSystem)

#pragma comment(lib, "wbemuuid.lib")

namespace remote_hands::sysinfo {

namespace {

// UTF-16 → UTF-8 conversion for outputs that go to JSON.
std::string narrow(const wchar_t* w, std::size_t len) {
    if (len == 0) return {};
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, w, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

std::string narrow(const std::wstring& w) {
    return narrow(w.c_str(), w.size());
}

// RAII wrapper for a process token handle.
struct TokenHandle {
    HANDLE h = nullptr;
    ~TokenHandle() { if (h) CloseHandle(h); }
    TokenHandle() = default;
    TokenHandle(const TokenHandle&) = delete;
    TokenHandle& operator=(const TokenHandle&) = delete;
};

bool open_process_token(TokenHandle& out, DWORD access) {
    return OpenProcessToken(GetCurrentProcess(), access, &out.h) != 0;
}

// Pulls a TOKEN_INFORMATION_CLASS payload of variable size into a vector<BYTE>.
std::vector<BYTE> query_token(HANDLE token, TOKEN_INFORMATION_CLASS klass) {
    DWORD needed = 0;
    GetTokenInformation(token, klass, nullptr, 0, &needed);
    if (needed == 0) return {};
    std::vector<BYTE> buf(needed);
    if (!GetTokenInformation(token, klass, buf.data(), needed, &needed)) {
        return {};
    }
    return buf;
}

}  // namespace

std::string arch() {
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    // PROCESSOR_ARCHITECTURE_ARM64 (12) was added in later SDK versions;
    // guard for v141_xp toolset which uses the v7.1A Platform SDK.
#ifndef PROCESSOR_ARCHITECTURE_ARM64
#define PROCESSOR_ARCHITECTURE_ARM64 12
#endif
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: return "x64";
        case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
        case PROCESSOR_ARCHITECTURE_ARM64: return "arm64";
        case PROCESSOR_ARCHITECTURE_ARM:   return "arm";
        default:                           return "unknown";
    }
}

std::string hostname() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD len = static_cast<DWORD>(std::size(buf));
    if (!GetComputerNameW(buf, &len)) return {};
    return narrow(buf, len);
}

namespace {

// Read a REG_SZ value from HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion.
// Returns empty string if absent / wrong type. KEY_WOW64_64KEY so a 32-bit
// agent on 64-bit Windows still reads the real (non-redirected) hive.
std::string read_cv_string(const wchar_t* value_name) {
    HKEY hkey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                      0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hkey)
            != ERROR_SUCCESS) {
        return {};
    }
    wchar_t buf[256] = {};
    DWORD   bytes = sizeof(buf) - sizeof(wchar_t);  // leave room for NUL
    DWORD   type  = 0;
    const LONG rc = RegQueryValueExW(hkey, value_name, nullptr, &type,
                                     reinterpret_cast<LPBYTE>(buf), &bytes);
    RegCloseKey(hkey);
    if (rc != ERROR_SUCCESS || type != REG_SZ) return {};
    return narrow(buf, std::wcslen(buf));
}

}  // namespace

std::string os_name() {
    std::string name = read_cv_string(L"ProductName");
    return name.empty() ? std::string{"Windows"} : name;
}

std::string os_version() {
    // Win10 2009+ exposes the marketing id (e.g. "22H2") in DisplayVersion.
    std::string v = read_cv_string(L"DisplayVersion");
    if (v.empty()) v = read_cv_string(L"ReleaseId");   // Win10 1507–2004
    if (v.empty()) {
        // Last resort: "10.0.<build>" from CurrentBuild(Number).
        std::string build = read_cv_string(L"CurrentBuildNumber");
        if (build.empty()) build = read_cv_string(L"CurrentBuild");
        v = build.empty() ? std::string{"unknown"}
                           : ("10.0." + build);
    }
    return v;
}

std::string current_user() {
    wchar_t buf[UNLEN + 1] = {};
    DWORD len = static_cast<DWORD>(std::size(buf));
    if (!GetUserNameW(buf, &len)) return {};
    // GetUserNameW returns length INCLUDING the null terminator.
    if (len > 0) --len;
    return narrow(buf, len);
}

std::string integrity_level_from_token(HANDLE token) {
    return platform::get_integrity_level(token);
}

std::string integrity_level() {
    TokenHandle tok;
    if (!open_process_token(tok, TOKEN_QUERY)) return {};
    return integrity_level_from_token(tok.h);
}

bool uiaccess_enabled() {
    TokenHandle tok;
    if (!open_process_token(tok, TOKEN_QUERY)) return false;

    DWORD ui_access = 0;
    DWORD len = sizeof(ui_access);
    if (!GetTokenInformation(tok.h, TokenUIAccess, &ui_access, len, &len)) {
        return false;
    }
    return ui_access != 0;
}

std::vector<std::string> enabled_privileges() {
    std::vector<std::string> out;

    TokenHandle tok;
    if (!open_process_token(tok, TOKEN_QUERY)) return out;

    auto buf = query_token(tok.h, TokenPrivileges);
    if (buf.empty()) return out;

    const auto* tp = reinterpret_cast<const TOKEN_PRIVILEGES*>(buf.data());
    out.reserve(tp->PrivilegeCount);
    for (DWORD i = 0; i < tp->PrivilegeCount; ++i) {
        const auto& la = tp->Privileges[i];
        if ((la.Attributes & SE_PRIVILEGE_ENABLED) == 0 &&
            (la.Attributes & SE_PRIVILEGE_ENABLED_BY_DEFAULT) == 0) {
            continue;
        }
        // Copy the LUID out of the const TOKEN_PRIVILEGES; LookupPrivilegeNameW
        // takes PLUID (non-const) even though it doesn't modify it.
        LUID    luid     = la.Luid;
        wchar_t name[64] = {};
        DWORD   name_len = static_cast<DWORD>(std::size(name));
        if (LookupPrivilegeNameW(nullptr, &luid, name, &name_len)) {
            out.push_back(narrow(name, name_len));
        }
    }
    return out;
}

bool enable_privilege(const wchar_t* privilege_name) {
    TokenHandle tok;
    if (!open_process_token(tok, TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY)) {
        return false;
    }

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, privilege_name, &luid)) {
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount           = 1;
    tp.Privileges[0].Luid       = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(tok.h, FALSE, &tp, sizeof(tp), nullptr, nullptr)) {
        return false;
    }
    return GetLastError() == ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Wake-timer support / VM detection (issue #82).
//
// `system.power.sleep` / `system.power.hibernate` `wake_at` arms a Win32
// waitable timer with `bResume=TRUE`. The API succeeds on a VM guest but the
// hypervisor generally does not de-allocate + resume the guest on the timer,
// so the machine never wakes. We therefore advertise
// `wake_timer_supported=false` on detected VMs. Detection is two-pronged and
// requires BOTH signals:
//   1. CPUID leaf 1, ECX bit 31 — the "hypervisor present" bit. Set by every
//      mainstream hypervisor (and never on bare metal).
//   2. WMI `Win32_ComputerSystem.Manufacturer` matches a known VM vendor.
// Requiring both avoids a false positive on bare-metal hosts that happen to
// have a hypervisor feature enabled but are not actually virtualised guests
// (and a false negative if a hypervisor masks the CPUID bit but the SMBIOS
// manufacturer still reads as a VM vendor — see the manufacturer-only OR
// below).

namespace {

bool cpuid_hypervisor_present() {
    // CPUID leaf 1: ECX bit 31 is the hypervisor-present bit. Reserved (always
    // 0) on physical hardware per Intel/AMD; set by VMware, Hyper-V, KVM/QEMU,
    // VirtualBox, Xen, Parallels.
    int regs[4] = {0, 0, 0, 0};
    __cpuid(regs, 1);
    return (static_cast<unsigned int>(regs[2]) & (1u << 31)) != 0;
}

std::string to_lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

// True if `manufacturer` (already lower-cased) contains any known VM-vendor
// substring. Covers the hypervisors listed in issue #82.
bool manufacturer_is_vm(const std::string& m) {
    static const char* kVendors[] = {
        "vmware", "virtualbox", "innotek",       // VirtualBox reports "innotek GmbH"
        "qemu", "microsoft corporation",          // Hyper-V guests report this
        "xen", "parallels", "kvm", "bochs",
        "red hat",                                // RHV / virtio
    };
    for (const char* v : kVendors) {
        if (m.find(v) != std::string::npos) return true;
    }
    return false;
}

// COM lifetime guard local to the WMI query. CoInitializeEx may return
// RPC_E_CHANGED_MODE if the calling thread already initialised a different
// apartment (init_capabilities runs on the main thread after ComInit); in
// that case we must NOT call CoUninitialize (we didn't take a reference).
struct ScopedCom {
    bool owned = false;
    explicit ScopedCom(DWORD model) {
        const HRESULT hr = CoInitializeEx(nullptr, model);
        owned = SUCCEEDED(hr);   // S_OK or S_FALSE => we hold a ref
    }
    ~ScopedCom() { if (owned) CoUninitialize(); }
    ScopedCom(const ScopedCom&) = delete;
    ScopedCom& operator=(const ScopedCom&) = delete;
};

template <class T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// Read Win32_ComputerSystem.Manufacturer via WMI. Returns "" on any failure
// (WMI not reachable, query blocked, etc.) — callers treat empty as "no VM
// vendor match", so the CPUID signal alone never flips the flag.
std::string wmi_manufacturer() {
    ScopedCom com(COINIT_MULTITHREADED);

    ComPtr<IWbemLocator> locator;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr,
                                CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                reinterpret_cast<void**>(&locator))) ||
        !locator) {
        return {};
    }

    ComPtr<IWbemServices> svc;
    {
        BSTR ns = SysAllocString(L"ROOT\\CIMV2");
        const HRESULT hr = locator->ConnectServer(
            ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc.p);
        SysFreeString(ns);
        if (FAILED(hr) || !svc) return {};
    }

    // Default proxy security blanket; failure is non-fatal (local queries
    // typically still succeed under the caller's identity).
    CoSetProxyBlanket(svc.p, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                      nullptr, EOAC_NONE);

    ComPtr<IEnumWbemClassObject> en;
    {
        BSTR lang  = SysAllocString(L"WQL");
        BSTR query = SysAllocString(
            L"SELECT Manufacturer FROM Win32_ComputerSystem");
        const HRESULT hr = svc->ExecQuery(
            lang, query,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr, &en.p);
        SysFreeString(lang);
        SysFreeString(query);
        if (FAILED(hr) || !en) return {};
    }

    IWbemClassObject* obj = nullptr;
    ULONG returned = 0;
    if (en->Next(WBEM_INFINITE, 1, &obj, &returned) != WBEM_S_NO_ERROR ||
        returned == 0 || obj == nullptr) {
        return {};
    }

    std::string result;
    VARIANT v;
    VariantInit(&v);
    if (SUCCEEDED(obj->Get(L"Manufacturer", 0, &v, nullptr, nullptr)) &&
        v.vt == VT_BSTR && v.bstrVal != nullptr) {
        const int len = static_cast<int>(SysStringLen(v.bstrVal));
        result = narrow(v.bstrVal, static_cast<std::size_t>(len));
    }
    VariantClear(&v);
    obj->Release();
    return result;
}

}  // namespace

bool wake_timer_supported() {
    // Compute once: the hardware identity does not change for a process
    // lifetime, and the WMI round-trip is comparatively expensive.
    static std::once_flag once;
    static bool cached = true;
    std::call_once(once, [] {
        const bool hv = cpuid_hypervisor_present();
        const std::string mfr = to_lower_ascii(wmi_manufacturer());
        const bool vendor_vm = !mfr.empty() && manufacturer_is_vm(mfr);
        // Per issue #82: a VM guest (wake timers unreliable) requires BOTH
        // the CPUID hypervisor-present bit AND a known VM-vendor SMBIOS
        // manufacturer string. Requiring both keeps the flag conservative —
        // the CPUID bit alone can be set on bare-metal hosts with nested-virt
        // features enabled, and the manufacturer string alone can read as a
        // vendor name (e.g. "Microsoft Corporation") on physical Surface
        // hardware. wake_timer_supported is the negation.
        const bool is_vm = hv && vendor_vm;
        cached = !is_vm;
    });
    return cached;
}

// ---------------------------------------------------------------------------
// OS input timings (issue #86).

InputSettings input_settings() {
    InputSettings s;

    s.double_click_time_ms = GetDoubleClickTime();
    s.double_click_w       = GetSystemMetrics(SM_CXDOUBLECLK);
    s.double_click_h       = GetSystemMetrics(SM_CYDOUBLECLK);

    // SPI_GETKEYBOARDDELAY: 0..3. Documented mapping is a linear 250 ms step
    // starting at 250 ms (0 => ~250 ms ... 3 => ~1000 ms).
    {
        DWORD delay_setting = 1;
        if (SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0,
                                  &delay_setting, 0)) {
            if (delay_setting > 3) delay_setting = 3;
            s.keyboard_repeat_delay_ms =
                250u + static_cast<unsigned int>(delay_setting) * 250u;
        }
    }

    // SPI_GETKEYBOARDSPEED: 0..31. Documented scale is ~2.5 cps (0) to ~30 cps
    // (31), approximately linear. Map to the nearest whole cps.
    {
        DWORD speed_setting = 31;
        if (SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0,
                                  &speed_setting, 0)) {
            if (speed_setting > 31) speed_setting = 31;
            const double cps =
                2.5 + (30.0 - 2.5) * (static_cast<double>(speed_setting) / 31.0);
            s.keyboard_repeat_rate_cps =
                static_cast<unsigned int>(cps + 0.5);
        }
    }

    // SPI_GETWHEELSCROLLLINES (XP+). WHEEL_PAGESCROLL (UINT_MAX) means
    // "scroll one page"; report that as absent rather than emitting a
    // sentinel the schema can't express. Pre-wheel hosts fail the call.
    {
        UINT lines = 0;
        if (SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0) &&
            lines != WHEEL_PAGESCROLL) {
            s.has_wheel_scroll_lines = true;
            s.wheel_scroll_lines     = lines;
        }
    }

    return s;
}

}  // namespace remote_hands::sysinfo

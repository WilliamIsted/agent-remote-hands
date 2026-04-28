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

#include <array>
#include <memory>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <lmcons.h>
#include <sddl.h>

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

std::string current_user() {
    wchar_t buf[UNLEN + 1] = {};
    DWORD len = static_cast<DWORD>(std::size(buf));
    if (!GetUserNameW(buf, &len)) return {};
    // GetUserNameW returns length INCLUDING the null terminator.
    if (len > 0) --len;
    return narrow(buf, len);
}

std::string integrity_level() {
    TokenHandle tok;
    if (!open_process_token(tok, TOKEN_QUERY)) return {};

    auto buf = query_token(tok.h, TokenIntegrityLevel);
    if (buf.empty()) return {};

    const auto* tml = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buf.data());
    PSID sid = tml->Label.Sid;
    const DWORD rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);

    if (rid <  SECURITY_MANDATORY_LOW_RID)         return "untrusted";
    if (rid <  SECURITY_MANDATORY_MEDIUM_RID)      return "low";
    if (rid <  SECURITY_MANDATORY_HIGH_RID)        return "medium";
    if (rid <  SECURITY_MANDATORY_SYSTEM_RID)      return "high";
    return "system";
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

}  // namespace remote_hands::sysinfo

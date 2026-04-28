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

#include "install.hpp"

#include "config.hpp"
#include "log.hpp"

#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

namespace remote_hands::install {

namespace {

constexpr const wchar_t* kTaskName         = L"AgentRemoteHands";
constexpr const wchar_t* kFirewallRuleName = L"Agent Remote Hands";
constexpr const wchar_t* kInstallSubdir    = L"AgentRemoteHands";
constexpr const wchar_t* kBinaryName       = L"remote-hands.exe";

// ---------------------------------------------------------------------------
// Path / privilege helpers

std::filesystem::path program_files_install_dir() {
    PWSTR path = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &path))) {
        out = std::filesystem::path{path} / kInstallSubdir;
        CoTaskMemFree(path);
    } else {
        out = std::filesystem::path{L"C:\\Program Files"} / kInstallSubdir;
    }
    return out;
}

std::filesystem::path current_executable() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path{buf};
}

bool is_running_as_admin() {
    BOOL is_admin = FALSE;
    PSID admin_group = nullptr;
    SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt_auth, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &admin_group)) {
        CheckTokenMembership(nullptr, admin_group, &is_admin);
        FreeSid(admin_group);
    }
    return is_admin != FALSE;
}

// ---------------------------------------------------------------------------
// Subprocess helper — runs `cmd /c <cmdline>` and returns its exit code.

int run_command(const std::wstring& cmdline) {
    std::wstring full = L"cmd.exe /c " + cmdline;

    STARTUPINFOW si{};
    si.cb            = sizeof(si);
    si.dwFlags       = STARTF_USESHOWWINDOW;
    si.wShowWindow   = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::wstring writable = full;
    const BOOL ok = CreateProcessW(
        nullptr, writable.data(),
        nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi);
    if (!ok) return -1;

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exit_code);
}

}  // namespace

// ---------------------------------------------------------------------------
// Install

int run_install(const Config& config) {
    if (!is_running_as_admin()) {
        std::wcerr << L"--install requires Administrator privileges\n";
        return 1;
    }

    const auto src = current_executable();
    if (src.empty()) {
        std::wcerr << L"Could not determine current executable path\n";
        return 1;
    }

    const auto install_dir = program_files_install_dir();
    std::error_code ec;
    std::filesystem::create_directories(install_dir, ec);
    if (ec) {
        std::wcerr << L"Failed to create " << install_dir.wstring() << L": "
                   << ec.message().c_str() << L"\n";
        return 1;
    }

    const auto dst = install_dir / kBinaryName;
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        std::wcerr << L"Failed to copy binary to " << dst.wstring()
                   << L" (error " << GetLastError() << L")\n";
        return 1;
    }
    log::info(L"Installed binary at %s", dst.c_str());

    // Firewall rule (best-effort — log on failure but don't abort install).
    {
        wchar_t cmd[512] = {};
        std::swprintf(cmd, std::size(cmd),
                      L"netsh advfirewall firewall add rule name=\"%s\" "
                      L"dir=in action=allow protocol=TCP localport=%u",
                      kFirewallRuleName,
                      static_cast<unsigned>(config.port));
        const int rc = run_command(cmd);
        if (rc == 0) log::info(L"Firewall rule added (TCP %u inbound)", config.port);
        else         log::warning(L"Firewall rule add returned %d (continuing)", rc);
    }

    // Task Scheduler logon-task.
    {
        std::wstring task_run = std::wstring{L"\\\""} + dst.wstring() + L"\\\"";
        if (config.discoverable) {
            task_run += L" --discoverable";
        }
        if (config.port != 8765) {
            wchar_t portbuf[16] = {};
            std::swprintf(portbuf, std::size(portbuf), L" --port %u",
                          static_cast<unsigned>(config.port));
            task_run += portbuf;
        }

        std::wstring cmd =
            L"schtasks /Create /TN \"" + std::wstring{kTaskName} +
            L"\" /TR \"" + task_run +
            L"\" /SC ONLOGON /RL HIGHEST /F";
        const int rc = run_command(cmd);
        if (rc != 0) {
            std::wcerr << L"schtasks /Create failed (rc=" << rc << L")\n";
            return 1;
        }
        log::info(L"Task Scheduler logon-task '%s' registered", kTaskName);
    }

    log::info(L"Install complete. The agent will autostart on the next user logon.");
    log::info(L"Run it now manually for this session: %s", dst.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// Uninstall

int run_uninstall(const Config& /*config*/) {
    if (!is_running_as_admin()) {
        std::wcerr << L"--uninstall requires Administrator privileges\n";
        return 1;
    }

    // Task Scheduler entry (best-effort — may not exist).
    {
        const std::wstring cmd =
            L"schtasks /Delete /TN \"" + std::wstring{kTaskName} + L"\" /F";
        const int rc = run_command(cmd);
        if (rc == 0) log::info(L"Task Scheduler task '%s' removed", kTaskName);
        else         log::info(L"Task Scheduler task '%s' was not present", kTaskName);
    }

    // Firewall rule (best-effort).
    {
        const std::wstring cmd =
            L"netsh advfirewall firewall delete rule name=\""
            + std::wstring{kFirewallRuleName} + L"\"";
        const int rc = run_command(cmd);
        if (rc == 0) log::info(L"Firewall rule removed");
        else         log::info(L"Firewall rule was not present");
    }

    // Binary + install directory.
    const auto install_dir = program_files_install_dir();
    const auto dst         = install_dir / kBinaryName;

    std::error_code ec;
    std::filesystem::remove(dst, ec);
    if (ec) {
        log::warning(L"Could not remove %s: %hs", dst.c_str(), ec.message().c_str());
    } else {
        log::info(L"Removed binary %s", dst.c_str());
    }

    // Try the install dir too (will only succeed if empty).
    std::filesystem::remove(install_dir, ec);

    log::info(L"Uninstall complete.");
    return 0;
}

}  // namespace remote_hands::install

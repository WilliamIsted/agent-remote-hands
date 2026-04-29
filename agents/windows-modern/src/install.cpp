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
// Task-XML builder
//
// We generate Task Scheduler XML directly (rather than `schtasks /Create
// /SC ONLOGON ...`) so we can include <RestartOnFailure>. schtasks's CLI
// flags don't expose those settings — XML import is the only path. The
// effective trigger / principal / run-level are equivalent to `/SC ONLOGON
// /RL HIGHEST` (LogonTrigger fires on any logon, Principal references the
// BUILTIN\Users SID so the task runs in the logged-on user's context with
// HighestAvailable elevation).

std::wstring xml_escape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        switch (c) {
            case L'&':  out += L"&amp;";  break;
            case L'<':  out += L"&lt;";   break;
            case L'>':  out += L"&gt;";   break;
            case L'"':  out += L"&quot;"; break;
            case L'\'': out += L"&apos;"; break;
            default:    out += c;
        }
    }
    return out;
}

std::wstring build_task_xml(const std::wstring& binary,
                            const std::wstring& args) {
    std::wstring xml = LR"(<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo>
    <Description>Agent Remote Hands wire-protocol agent (autostart on user logon).</Description>
    <Author>Agent Remote Hands</Author>
  </RegistrationInfo>
  <Triggers>
    <LogonTrigger>
      <Enabled>true</Enabled>
    </LogonTrigger>
  </Triggers>
  <Principals>
    <Principal id="Author">
      <GroupId>S-1-5-32-545</GroupId>
      <RunLevel>HighestAvailable</RunLevel>
    </Principal>
  </Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <AllowHardTerminate>true</AllowHardTerminate>
    <StartWhenAvailable>true</StartWhenAvailable>
    <Enabled>true</Enabled>
    <Hidden>false</Hidden>
    <RunOnlyIfIdle>false</RunOnlyIfIdle>
    <AllowStartOnDemand>true</AllowStartOnDemand>
    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
    <Priority>7</Priority>
    <RestartOnFailure>
      <Interval>PT1M</Interval>
      <Count>3</Count>
    </RestartOnFailure>
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>{BINARY}</Command>
      <Arguments>{ARGS}</Arguments>
    </Exec>
  </Actions>
</Task>)";

    auto replace_first = [&](std::wstring_view placeholder,
                             const std::wstring& value) {
        const auto pos = xml.find(placeholder);
        if (pos != std::wstring::npos) {
            xml.replace(pos, placeholder.size(), value);
        }
    };
    replace_first(L"{BINARY}", xml_escape(binary));
    replace_first(L"{ARGS}",   xml_escape(args));
    return xml;
}

// Writes `xml` to a UTF-16LE file (with BOM) under %TEMP%. Caller deletes.
bool write_task_xml(const std::wstring& xml, std::filesystem::path& out_path) {
    wchar_t temp_dir[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, temp_dir) == 0) return false;

    out_path = std::filesystem::path{temp_dir} / L"agent-remote-hands-task.xml";

    HANDLE h = CreateFileW(out_path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const unsigned char bom[] = {0xff, 0xfe};
    if (!WriteFile(h, bom, sizeof(bom), &written, nullptr) ||
        !WriteFile(h, xml.data(),
                   static_cast<DWORD>(xml.size() * sizeof(wchar_t)),
                   &written, nullptr)) {
        CloseHandle(h);
        return false;
    }
    CloseHandle(h);
    return true;
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

    // Task Scheduler logon-task. Imported from generated XML so we can set
    // <RestartOnFailure> — schtasks's CLI flags don't expose that.
    {
        std::wstring args;
        if (config.discoverable) args += L"--discoverable";
        if (config.port != 8765) {
            if (!args.empty()) args += L' ';
            wchar_t portbuf[16] = {};
            std::swprintf(portbuf, std::size(portbuf), L"--port %u",
                          static_cast<unsigned>(config.port));
            args += portbuf;
        }

        const std::wstring xml = build_task_xml(dst.wstring(), args);
        std::filesystem::path xml_path;
        if (!write_task_xml(xml, xml_path)) {
            std::wcerr << L"Failed to write task XML to %TEMP%\n";
            return 1;
        }

        std::wstring cmd =
            L"schtasks /Create /TN \"" + std::wstring{kTaskName} +
            L"\" /XML \"" + xml_path.wstring() + L"\" /F";
        const int rc = run_command(cmd);

        std::error_code ec_remove;
        std::filesystem::remove(xml_path, ec_remove);   // best-effort cleanup

        if (rc != 0) {
            std::wcerr << L"schtasks /Create /XML failed (rc=" << rc << L")\n";
            return 1;
        }
        log::info(L"Task Scheduler logon-task '%s' registered "
                  L"(restart-on-failure: 3 attempts, 1 minute apart)",
                  kTaskName);
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

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

#include "uipi.hpp"

#include "connection.hpp"
#include "errors.hpp"
#include "json.hpp"
#include "protocol.hpp"
#include "sysinfo.hpp"

#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>

namespace remote_hands::uipi {

namespace {

// Maps a SECURITY_MANDATORY_*_RID to the integrity-level token used on the
// wire. Returns empty for unrecognised RIDs.
const char* rid_to_string(DWORD rid) {
    if (rid <  SECURITY_MANDATORY_LOW_RID)         return "untrusted";
    if (rid <  SECURITY_MANDATORY_MEDIUM_RID)      return "low";
    if (rid <  SECURITY_MANDATORY_HIGH_RID)        return "medium";
    if (rid <  SECURITY_MANDATORY_SYSTEM_RID)      return "high";
    return "system";
}

int integrity_rank(const std::string& il) {
    if (il == "untrusted") return 0;
    if (il == "low")       return 1;
    if (il == "medium")    return 2;
    if (il == "high")      return 3;
    if (il == "system")    return 4;
    return -1;
}

std::string read_token_il(HANDLE token) {
    DWORD needed = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &needed);
    if (needed == 0) return {};

    std::vector<BYTE> buf(needed);
    if (!GetTokenInformation(token, TokenIntegrityLevel, buf.data(),
                             needed, &needed)) {
        return {};
    }

    const auto* tml = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buf.data());
    PSID sid = tml->Label.Sid;
    const DWORD rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
    return rid_to_string(rid);
}

}  // namespace

const std::string& agent_integrity() {
    // Memoised at first call; the agent's own IL doesn't change at runtime.
    static const std::string il = sysinfo::integrity_level();
    return il;
}

std::string window_integrity(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return {};

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return {};

    HANDLE hproc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hproc) return {};

    HANDLE htok = nullptr;
    if (!OpenProcessToken(hproc, TOKEN_QUERY, &htok)) {
        CloseHandle(hproc);
        return {};
    }

    auto il = read_token_il(htok);
    CloseHandle(htok);
    CloseHandle(hproc);
    return il;
}

bool input_allowed(const std::string& agent_il, const std::string& target_il) {
    if (agent_il.empty() || target_il.empty()) return true;

    const int agent_rank  = integrity_rank(agent_il);
    const int target_rank = integrity_rank(target_il);
    if (agent_rank < 0 || target_rank < 0) return true;

    return agent_rank >= target_rank;
}

namespace {

void emit_uipi_block(Connection& conn,
                     const std::string& agent_il,
                     const std::string& target_il) {
    std::string detail = "{";
    json::append_kv_string(detail, "agent_il",  agent_il);   detail += ',';
    json::append_kv_string(detail, "target_il", target_il);
    detail += '}';
    conn.writer().write_err(ErrorCode::UipiBlocked, detail);
}

}  // namespace

bool check_foreground_or_fail(Connection& conn) {
    HWND fg = GetForegroundWindow();
    if (!fg) return true;  // nothing focused; let the verb run, OS will sort it

    const auto target_il = window_integrity(fg);
    const auto& self_il  = agent_integrity();

    if (input_allowed(self_il, target_il)) return true;

    emit_uipi_block(conn, self_il, target_il);
    return false;
}

bool check_window_or_fail(Connection& conn, HWND target) {
    if (!target) return true;

    const auto target_il = window_integrity(target);
    const auto& self_il  = agent_integrity();

    if (input_allowed(self_il, target_il)) return true;

    emit_uipi_block(conn, self_il, target_il);
    return false;
}

}  // namespace remote_hands::uipi

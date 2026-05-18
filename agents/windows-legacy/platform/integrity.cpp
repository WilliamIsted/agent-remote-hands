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

// windows-legacy integrity-level seam.
// XP has no mandatory integrity policy (TokenIntegrityLevel is Vista+), so we
// return the conventional "medium" on XP.  Vista and later use the same
// GetTokenInformation + RID-decode logic as the modern build.

#include "platform.hpp"

#include <vector>
#include <versionhelpers.h>

namespace remote_hands::platform {

std::string get_integrity_level(HANDLE token) {
    if (!IsWindowsVistaOrGreater()) {
        return "medium";
    }

    DWORD needed = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &needed);
    if (needed == 0) return {};
    std::vector<BYTE> buf(needed);
    if (!GetTokenInformation(token, TokenIntegrityLevel,
                             buf.data(), needed, &needed)) {
        return {};
    }

    const auto* tml = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buf.data());
    PSID sid = tml->Label.Sid;
    const DWORD rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);

    if (rid <  SECURITY_MANDATORY_LOW_RID)    return "untrusted";
    if (rid <  SECURITY_MANDATORY_MEDIUM_RID) return "low";
    if (rid <  SECURITY_MANDATORY_HIGH_RID)   return "medium";
    if (rid <  SECURITY_MANDATORY_SYSTEM_RID) return "high";
    return "system";
}

}  // namespace remote_hands::platform

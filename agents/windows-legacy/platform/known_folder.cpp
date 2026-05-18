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

// windows-legacy known-folder seam.
// SHGetKnownFolderPath is Vista+; XP uses CSIDL_COMMON_APPDATA via
// SHGetFolderPathW (available from Win2000+).

#include "platform.hpp"

#include <shlobj.h>

namespace remote_hands::platform {

std::wstring get_program_data_path() {
    wchar_t path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(
            nullptr, CSIDL_COMMON_APPDATA, nullptr,
            SHGFP_TYPE_CURRENT, path))) {
        return path;
    }
    return L"C:\\ProgramData";
}

}  // namespace remote_hands::platform

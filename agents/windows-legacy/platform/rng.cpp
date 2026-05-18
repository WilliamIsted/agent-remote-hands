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

#include "platform.hpp"

#include <stdexcept>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>

namespace remote_hands::platform {

std::vector<uint8_t> generate_random_bytes(std::size_t n) {
    HCRYPTPROV prov = 0;
    if (!CryptAcquireContext(&prov, nullptr, nullptr,
                             PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        throw std::runtime_error("CryptAcquireContext failed");
    }
    std::vector<uint8_t> buf(n);
    const BOOL ok = CryptGenRandom(prov, static_cast<DWORD>(n), buf.data());
    CryptReleaseContext(prov, 0);
    if (!ok) {
        throw std::runtime_error("CryptGenRandom failed");
    }
    return buf;
}

}  // namespace remote_hands::platform

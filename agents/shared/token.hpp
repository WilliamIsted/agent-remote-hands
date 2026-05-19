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

#pragma once

// Elevation token file management.
//
// On agent start, a 256-bit random token is written to the configured token
// file path (default %ProgramData%\AgentRemoteHands\token). The file is ACL'd
// so only Administrators and the agent's run-as account can read it.
//
// To raise their tier, a caller reads the file (which requires filesystem
// access to the agent host) and quotes the contents in
// `connection.tier_raise <tier> <token>`. The agent compares the presented
// token to the in-memory copy with a constant-time comparison.
//
// Token rotation policy is controlled by ttl_hours (see initialise()):
//   -1  FOREVER  — reuse the existing token across restarts indefinitely
//    0  per-session — rotate on every start; delete the file on agent exit
//   >0  hours    — reuse if the file is younger than ttl_hours, else rotate
//                  Default: 720 (30 days)

#include <filesystem>
#include <string>
#include <string_view>

namespace remote_hands {

class TokenStore {
public:
    // Applies the token TTL policy at `path`:
    //   ttl_hours == -1 (FOREVER): load existing if present, else generate.
    //   ttl_hours ==  0 (per-session): always rotate; file is deleted on destruction.
    //   ttl_hours  >  0 (hours): reuse if younger than ttl_hours, else rotate.
    // The file is always deleted then re-created on rotation (never overwritten
    // in-place) so the filesystem creation timestamp matches the issue time.
    // Throws on filesystem error.
    static TokenStore initialise(const std::filesystem::path& path, int ttl_hours);

    // Constant-time comparison of the presented token to the stored value.
    bool verify(std::string_view presented) const noexcept;

    // The path the token lives at, for diagnostic logging.
    const std::filesystem::path& path() const noexcept { return path_; }

    // Deletes the token file. Called automatically on destruction when
    // ttl_hours == 0 (per-session). Safe to call manually; idempotent.
    void destroy() const noexcept;

    ~TokenStore();
    // Move transfers file-deletion responsibility; the moved-from instance
    // must not delete the file, so per_session_ is cleared on the source.
    TokenStore(TokenStore&& other) noexcept;
    TokenStore& operator=(TokenStore&& other) noexcept;

private:
    TokenStore(std::filesystem::path path, std::string token, bool per_session)
        : path_{std::move(path)}, token_{std::move(token)},
          per_session_{per_session} {}

    std::filesystem::path  path_;
    std::string            token_;       // hex-encoded, 64 chars (256 bits)
    bool                   per_session_; // delete file on destruction
};

}  // namespace remote_hands

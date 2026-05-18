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
// On agent start, a 256-bit random token is generated and written to the
// configured token file path (default %ProgramData%\AgentRemoteHands\token).
// The file is ACL'd so only Administrators and the agent's run-as account
// can read it.
//
// To raise their tier, a caller reads the file (which requires filesystem
// access to the agent host) and quotes the contents in
// `connection.tier_raise <tier> <token>`. The agent compares the presented
// token to the in-memory copy with a constant-time comparison.
//
// Tokens rotate on agent restart; existing connections keep their tier
// across the rotation, but new elevations require the new token.

#include <filesystem>
#include <string>
#include <string_view>

namespace remote_hands {

class TokenStore {
public:
    // Loads or generates the token at `path`. Creates parent directories
    // and the file if absent; rewrites the file on every start so the
    // token rotates per process launch. Throws on filesystem error.
    static TokenStore initialise(const std::filesystem::path& path);

    // Constant-time comparison of the presented token to the stored value.
    bool verify(std::string_view presented) const noexcept;

    // The path the token lives at, for diagnostic logging.
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    TokenStore(std::filesystem::path path, std::string token)
        : path_{std::move(path)}, token_{std::move(token)} {}

    std::filesystem::path  path_;
    std::string            token_;     // hex-encoded, 64 chars (256 bits)
};

}  // namespace remote_hands

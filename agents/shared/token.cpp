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

#include "token.hpp"

#include "log.hpp"
#include "platform.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace remote_hands {

namespace {

constexpr std::size_t kTokenBytes = 32;          // 256 bits

std::string generate_hex_token() {
    const auto raw = platform::generate_random_bytes(kTokenBytes);
    if (raw.size() != kTokenBytes) {
        throw std::runtime_error("generate_random_bytes returned wrong size");
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (auto b : raw) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0f]);
    }
    return out;
}

// Delete the file at path; ignore errors (file may not exist).
void remove_file(const std::filesystem::path& path) noexcept {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// Delete-then-create so the filesystem creation timestamp reflects this
// rotation rather than a prior one (overwriting in-place preserves the
// original ftCreationTime on NTFS).
// ACL hardening is applied by the installer; we only write the contents.
void write_token_file(const std::filesystem::path& path, std::string_view token) {
    std::filesystem::create_directories(path.parent_path());
    remove_file(path);
    std::ofstream out{path, std::ios::binary};
    if (!out) throw std::runtime_error("failed to open token file for writing");
    out.write(token.data(), static_cast<std::streamsize>(token.size()));
    if (!out) throw std::runtime_error("failed to write token file");
}

// Returns the 64-char hex token from an existing file, or empty string if
// the file is absent, unreadable, or has an unexpected size.
std::string load_token_file(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) return {};
    std::string token(kTokenBytes * 2, '\0');
    in.read(token.data(), static_cast<std::streamsize>(token.size()));
    if (static_cast<std::size_t>(in.gcount()) != kTokenBytes * 2) return {};
    return token;
}

// Returns true if the token file exists and its last-write time is younger
// than ttl_hours hours. Uses file_time_type::clock throughout to avoid
// cross-clock conversion (not available until C++20 clock_cast).
bool token_within_ttl(const std::filesystem::path& path, int ttl_hours) {
    std::error_code ec;
    const auto ftime = std::filesystem::last_write_time(path, ec);
    if (ec) return false;
    const auto now   = std::filesystem::file_time_type::clock::now();
    const auto age_h = std::chrono::duration_cast<std::chrono::hours>(now - ftime).count();
    return age_h < static_cast<long long>(ttl_hours);
}

bool constant_time_equal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    unsigned int diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned int>(static_cast<unsigned char>(a[i]) ^
                                          static_cast<unsigned char>(b[i]));
    }
    return diff == 0;
}

}  // namespace

TokenStore TokenStore::initialise(const std::filesystem::path& path, int ttl_hours) {
    std::string token;
    bool per_session = (ttl_hours == 0);

    if (ttl_hours == 0) {
        // Per-session: always rotate; file is deleted on destruction.
        token = generate_hex_token();
        write_token_file(path, token);
        log::info(L"Token issued (per-session) at %s", path.c_str());

    } else if (ttl_hours == -1) {
        // FOREVER: reuse the existing token indefinitely; only rotate if absent.
        token = load_token_file(path);
        if (token.empty()) {
            token = generate_hex_token();
            write_token_file(path, token);
            log::info(L"Token issued (permanent) at %s", path.c_str());
        } else {
            log::info(L"Token reused (permanent) at %s", path.c_str());
        }

    } else {
        // Fixed TTL: reuse if the file is younger than ttl_hours hours.
        if (token_within_ttl(path, ttl_hours)) {
            token = load_token_file(path);
        }
        if (token.empty()) {
            // File missing, expired, or unreadable after the TTL check passed
            // (e.g. corruption, race) — rotate.
            token = generate_hex_token();
            write_token_file(path, token);
            log::info(L"Token rotated (TTL %dh) at %s", ttl_hours, path.c_str());
        } else {
            log::info(L"Token reused (TTL %dh) at %s", ttl_hours, path.c_str());
        }
    }

    return TokenStore{path, std::move(token), per_session};
}

TokenStore::TokenStore(TokenStore&& other) noexcept
    : path_{std::move(other.path_)},
      token_{std::move(other.token_)},
      per_session_{other.per_session_}
{
    other.per_session_ = false;
}

TokenStore& TokenStore::operator=(TokenStore&& other) noexcept {
    if (this != &other) {
        if (per_session_) destroy();
        path_        = std::move(other.path_);
        token_       = std::move(other.token_);
        per_session_ = other.per_session_;
        other.per_session_ = false;
    }
    return *this;
}

void TokenStore::destroy() const noexcept {
    remove_file(path_);
}

TokenStore::~TokenStore() {
    if (per_session_) {
        // Don't log from the destructor — the log subsystem may already be
        // torn down at this point in the shutdown sequence.
        destroy();
    }
}

bool TokenStore::verify(std::string_view presented) const noexcept {
    return constant_time_equal(presented, token_);
}

}  // namespace remote_hands

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

#include <fstream>
#include <stdexcept>

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

void write_token_file(const std::filesystem::path& path, std::string_view token) {
    std::filesystem::create_directories(path.parent_path());

    // Note: ACL hardening is applied by the installer (--install path) where
    // the token directory is created with restrictive ACLs. Here we just
    // overwrite the contents.
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) {
        throw std::runtime_error("failed to open token file for writing");
    }
    out.write(token.data(), static_cast<std::streamsize>(token.size()));
    if (!out) {
        throw std::runtime_error("failed to write token file");
    }
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

TokenStore TokenStore::initialise(const std::filesystem::path& path) {
    auto token = generate_hex_token();
    write_token_file(path, token);
    log::info(L"Token file rotated at %s", path.c_str());
    return TokenStore{path, std::move(token)};
}

bool TokenStore::verify(std::string_view presented) const noexcept {
    return constant_time_equal(presented, token_);
}

}  // namespace remote_hands

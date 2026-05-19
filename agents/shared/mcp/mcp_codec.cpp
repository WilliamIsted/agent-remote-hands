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

#include "mcp_codec.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace remote_hands::mcp {

namespace {

constexpr std::size_t kReadChunkBytes  = 4096;
// Defensive ceiling on a single MCP frame. The wire is unauthenticated; a
// hostile peer must not be able to drive unbounded allocation via a forged
// Content-Length. 64 MiB comfortably exceeds any legitimate verb body the
// flat-arg Phase 1 surface produces (large base64 payloads are Phase 2).
constexpr std::size_t kMaxFrameBytes   = 64ull * 1024 * 1024;
constexpr std::size_t kMaxHeaderBytes  = 8192;

}  // namespace

McpCodec::McpCodec(SOCKET socket, std::vector<std::byte> residual)
    : socket_{socket}, buffer_{std::move(residual)} {}

bool McpCodec::fill_from_socket() {
    std::byte chunk[kReadChunkBytes];
    const int n = recv(socket_,
                       reinterpret_cast<char*>(chunk),
                       static_cast<int>(kReadChunkBytes),
                       0);
    if (n == 0) return false;          // graceful EOF
    if (n < 0) {
        throw std::runtime_error("recv() failed (mcp)");
    }
    buffer_.insert(buffer_.end(), chunk, chunk + n);
    return true;
}

std::string McpCodec::read_exact(std::size_t n) {
    while (buffer_.size() < n) {
        if (!fill_from_socket()) {
            throw std::runtime_error("connection closed mid-MCP-frame");
        }
    }
    std::string out(reinterpret_cast<const char*>(buffer_.data()), n);
    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(n));
    return out;
}

std::optional<std::string> McpCodec::try_extract_header_line() {
    // Find CRLF.
    for (std::size_t i = 0; i + 1 < buffer_.size(); ++i) {
        if (buffer_[i] == std::byte{'\r'} && buffer_[i + 1] == std::byte{'\n'}) {
            std::string line(reinterpret_cast<const char*>(buffer_.data()), i);
            buffer_.erase(buffer_.begin(),
                          buffer_.begin() + static_cast<std::ptrdiff_t>(i + 2));
            return line;
        }
    }
    return std::nullopt;
}

std::optional<std::string> McpCodec::read_frame() {
    // --- Header block: lines terminated by CRLF, ended by an empty line. ---
    long long content_length = -1;
    while (true) {
        std::optional<std::string> line;
        while (!(line = try_extract_header_line()).has_value()) {
            if (buffer_.size() > kMaxHeaderBytes) {
                throw std::runtime_error("MCP header block too large");
            }
            if (!fill_from_socket()) {
                // Clean EOF only valid if nothing is buffered (no partial
                // header); otherwise the peer truncated a frame.
                if (buffer_.empty() && content_length < 0) {
                    return std::nullopt;
                }
                throw std::runtime_error("connection closed mid-MCP-header");
            }
        }
        if (line->empty()) {
            break;  // end of header block
        }
        // Parse "Key: Value". Case-insensitive key match for Content-Length.
        const auto colon = line->find(':');
        if (colon == std::string::npos) {
            throw std::runtime_error("malformed MCP header line");
        }
        std::string key = line->substr(0, colon);
        std::string val = line->substr(colon + 1);
        // Trim surrounding whitespace from value.
        const auto ws_begin = val.find_first_not_of(" \t");
        const auto ws_end   = val.find_last_not_of(" \t");
        if (ws_begin == std::string::npos) {
            val.clear();
        } else {
            val = val.substr(ws_begin, ws_end - ws_begin + 1);
        }
        std::string key_lower = key;
        std::transform(key_lower.begin(), key_lower.end(), key_lower.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        if (key_lower == "content-length") {
            if (val.empty()) {
                throw std::runtime_error("invalid Content-Length");
            }
            content_length = 0;
            for (char c : val) {
                if (c < '0' || c > '9') {
                    throw std::runtime_error("invalid Content-Length");
                }
                content_length = content_length * 10 + (c - '0');
                if (content_length > static_cast<long long>(kMaxFrameBytes)) {
                    throw std::runtime_error("MCP frame exceeds size ceiling");
                }
            }
        }
        // Content-Type and any other headers are ignored (LSP convention).
    }

    if (content_length < 0) {
        throw std::runtime_error("MCP frame missing Content-Length");
    }
    if (content_length == 0) {
        return std::string{};
    }
    return read_exact(static_cast<std::size_t>(content_length));
}

void McpCodec::write_frame(const std::string& json) {
    char header[64];
    const int n = std::snprintf(header, sizeof(header),
                                "Content-Length: %zu\r\n\r\n", json.size());
    if (n <= 0) {
        throw std::runtime_error("MCP header format failed");
    }

    // Send header then body. Coalesced into one buffer to avoid a tiny
    // header packet preceding every frame.
    std::string frame;
    frame.reserve(static_cast<std::size_t>(n) + json.size());
    frame.append(header, static_cast<std::size_t>(n));
    frame.append(json);

    const char* p     = frame.data();
    std::size_t left  = frame.size();
    while (left > 0) {
        const int chunk = static_cast<int>(
            std::min<std::size_t>(left, 1 << 20));
        const int sent = send(socket_, p, chunk, 0);
        if (sent <= 0) {
            throw std::runtime_error("send() failed (mcp)");
        }
        p    += sent;
        left -= static_cast<std::size_t>(sent);
    }
}

}  // namespace remote_hands::mcp

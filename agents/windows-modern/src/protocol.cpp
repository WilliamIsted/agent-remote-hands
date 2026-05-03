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

#include "protocol.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>

namespace remote_hands::wire {

namespace {

constexpr std::size_t kReadChunkBytes = 4096;

}  // namespace

Request tokenize_header(std::string_view line) {
    Request req;
    const std::size_t n = line.size();
    std::size_t i = 0;
    while (i < n) {
        // Skip leading spaces
        while (i < n && line[i] == ' ') ++i;
        if (i >= n) break;

        std::string token;
        if (line[i] == '"') {
            // Quoted token: literal until the next '"'. No escape mechanism.
            ++i;  // skip opening quote
            const std::size_t start = i;
            while (i < n && line[i] != '"') ++i;
            if (i >= n) {
                // Unmatched opening quote — surface as a parse error so the
                // dispatcher can emit ERR invalid_args. We deliberately do
                // NOT throw here; the framing layer is fine, only the args
                // are malformed.
                req.parse_error = "unmatched quote in header";
                req.verb.clear();
                req.args.clear();
                return req;
            }
            token.assign(line.substr(start, i - start));
            ++i;  // skip closing quote
        } else {
            // Unquoted token: read until the next space.
            const std::size_t start = i;
            while (i < n && line[i] != ' ') ++i;
            token.assign(line.substr(start, i - start));
        }

        if (req.verb.empty() && req.args.empty()) {
            // First token is the verb. We disallow a quoted verb (no real
            // use case; verb names are dotted ASCII identifiers).
            req.verb = std::move(token);
        } else {
            req.args.emplace_back(std::move(token));
        }
    }
    return req;
}

// ---------------------------------------------------------------------------
// Reader

Reader::Reader(SOCKET socket) : socket_{socket} {
    buffer_.reserve(kReadChunkBytes);
}

bool Reader::fill_from_socket() {
    std::byte chunk[kReadChunkBytes];
    const int n = recv(socket_,
                       reinterpret_cast<char*>(chunk),
                       static_cast<int>(kReadChunkBytes),
                       0);
    if (n == 0) return false;          // graceful EOF
    if (n < 0) {
        throw std::runtime_error("recv() failed");
    }
    buffer_.insert(buffer_.end(), chunk, chunk + n);
    return true;
}

std::optional<std::string> Reader::try_extract_line() {
    auto it = std::find(buffer_.begin(), buffer_.end(), std::byte{'\n'});
    if (it == buffer_.end()) return std::nullopt;

    const std::size_t line_len = static_cast<std::size_t>(it - buffer_.begin());
    std::string line(reinterpret_cast<const char*>(buffer_.data()), line_len);
    // Strip trailing \r if present.
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    // Drop the consumed bytes (line + the \n).
    buffer_.erase(buffer_.begin(), it + 1);
    return line;
}

std::optional<Request> Reader::read_header() {
    while (true) {
        if (auto line = try_extract_line(); line.has_value()) {
            if (line->size() > kMaxHeaderLineBytes) {
                throw std::runtime_error("header line exceeds max size");
            }
            return tokenize_header(*line);
        }
        // Guard against runaway buffers from malformed clients.
        if (buffer_.size() > kMaxHeaderLineBytes) {
            throw std::runtime_error("unterminated header line");
        }
        if (!fill_from_socket()) {
            // Clean EOF only valid if buffer_ is empty; otherwise truncation.
            if (!buffer_.empty()) {
                throw std::runtime_error("connection closed mid-header");
            }
            return std::nullopt;
        }
    }
}

std::vector<std::byte> Reader::read_payload(std::size_t length) {
    std::vector<std::byte> out;
    out.reserve(length);

    // Drain the buffer first.
    if (!buffer_.empty()) {
        const std::size_t take = std::min(length, buffer_.size());
        out.insert(out.end(), buffer_.begin(), buffer_.begin() + take);
        buffer_.erase(buffer_.begin(), buffer_.begin() + take);
    }

    // Top up from the socket.
    while (out.size() < length) {
        std::byte chunk[kReadChunkBytes];
        const std::size_t want =
            std::min(kReadChunkBytes, length - out.size());
        const int n = recv(socket_,
                           reinterpret_cast<char*>(chunk),
                           static_cast<int>(want),
                           0);
        if (n == 0) {
            throw std::runtime_error("connection closed mid-payload");
        }
        if (n < 0) {
            throw std::runtime_error("recv() failed reading payload");
        }
        out.insert(out.end(), chunk, chunk + n);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Writer

Writer::Writer(SOCKET socket) : socket_{socket} {}

void Writer::write_raw(std::span<const std::byte> bytes) {
    const char* p = reinterpret_cast<const char*>(bytes.data());
    std::size_t remaining = bytes.size();
    while (remaining > 0) {
        const int chunk = static_cast<int>(std::min<std::size_t>(remaining, INT_MAX));
        const int n = send(socket_, p, chunk, 0);
        if (n <= 0) {
            throw std::runtime_error("send() failed");
        }
        p         += n;
        remaining -= static_cast<std::size_t>(n);
    }
}

void Writer::write_raw(std::string_view sv) {
    write_raw(std::span{
        reinterpret_cast<const std::byte*>(sv.data()), sv.size()});
}

void Writer::write_ok() {
    std::lock_guard lock{mutex_};
    write_raw(std::string_view{"OK 0\n"});
}

void Writer::write_ok(std::span<const std::byte> payload) {
    std::lock_guard lock{mutex_};
    char header[32];
    const int n = std::snprintf(header, sizeof(header),
                                "OK %zu\n", payload.size());
    if (n <= 0) throw std::runtime_error("header format failed");
    write_raw(std::string_view{header, static_cast<std::size_t>(n)});
    if (!payload.empty()) {
        write_raw(payload);
    }
}

void Writer::write_ok(std::string_view payload) {
    write_ok(std::span{
        reinterpret_cast<const std::byte*>(payload.data()), payload.size()});
}

void Writer::write_err(ErrorCode code) {
    std::lock_guard lock{mutex_};
    std::string line = "ERR ";
    line += to_wire(code);
    line += " 0\n";
    write_raw(line);
}

void Writer::write_err(ErrorCode code, std::string_view detail_json) {
    if (detail_json.empty()) {
        write_err(code);
        return;
    }
    std::lock_guard lock{mutex_};
    char prefix[64];
    const int n = std::snprintf(prefix, sizeof(prefix),
                                "ERR %.*s %zu\n",
                                static_cast<int>(to_wire(code).size()),
                                to_wire(code).data(),
                                detail_json.size());
    if (n <= 0) throw std::runtime_error("error header format failed");
    write_raw(std::string_view{prefix, static_cast<std::size_t>(n)});
    write_raw(detail_json);
}

void Writer::write_event(std::string_view subscription_id,
                         std::span<const std::byte> payload) {
    std::lock_guard lock{mutex_};
    char prefix[128];
    const int n = std::snprintf(prefix, sizeof(prefix),
                                "EVENT %.*s %zu\n",
                                static_cast<int>(subscription_id.size()),
                                subscription_id.data(),
                                payload.size());
    if (n <= 0) throw std::runtime_error("event header format failed");
    write_raw(std::string_view{prefix, static_cast<std::size_t>(n)});
    if (!payload.empty()) {
        write_raw(payload);
    }
}

void Writer::write_event(std::string_view subscription_id,
                         std::string_view payload) {
    write_event(subscription_id,
                std::span{reinterpret_cast<const std::byte*>(payload.data()),
                          payload.size()});
}

}  // namespace remote_hands::wire

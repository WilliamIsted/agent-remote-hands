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

#include "ws_codec.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace remote_hands::ws {

namespace {

constexpr std::size_t kReadChunkBytes  = 4096;
// Same defensive ceiling as McpCodec — a hostile peer must not be able to
// drive unbounded allocation via a forged length field.
constexpr std::size_t kMaxFrameBytes   = 64ull * 1024 * 1024;
// RFC 6455 §5.5: all control frames "MUST have a payload length of 125 bytes
// or less". Anything larger is a protocol violation.
constexpr std::size_t kMaxControlBytes = 125;

// RFC 6455 frame header bits.
constexpr std::uint8_t kBitFin  = 0x80;
constexpr std::uint8_t kBitRsv1 = 0x40;
constexpr std::uint8_t kBitRsv2 = 0x20;
constexpr std::uint8_t kBitRsv3 = 0x10;
constexpr std::uint8_t kMaskFlag    = 0x80;
constexpr std::uint8_t kInitialLenMask = 0x7F;

}  // namespace

WsCodec::WsCodec(SOCKET socket, std::vector<std::byte> residual)
    : socket_{socket}, buffer_{std::move(residual)} {}

bool WsCodec::fill_from_socket() {
    std::byte chunk[kReadChunkBytes];
    const int n = recv(socket_,
                       reinterpret_cast<char*>(chunk),
                       static_cast<int>(kReadChunkBytes),
                       0);
    if (n == 0) return false;          // graceful EOF
    if (n < 0) {
        throw std::runtime_error("recv() failed (ws)");
    }
    buffer_.insert(buffer_.end(), chunk, chunk + n);
    return true;
}

bool WsCodec::read_exact(std::size_t n, std::vector<std::byte>& out) {
    while (buffer_.size() < n) {
        if (!fill_from_socket()) {
            return false;             // EOF
        }
    }
    out.assign(buffer_.begin(),
               buffer_.begin() + static_cast<std::ptrdiff_t>(n));
    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(n));
    return true;
}

void WsCodec::send_all(const std::byte* p, std::size_t left) {
    while (left > 0) {
        const int chunk = static_cast<int>(
            std::min<std::size_t>(left, 1 << 20));
        const int sent = send(socket_,
                              reinterpret_cast<const char*>(p),
                              chunk, 0);
        if (sent <= 0) {
            throw std::runtime_error("send() failed (ws)");
        }
        p    += sent;
        left -= static_cast<std::size_t>(sent);
    }
}

void WsCodec::write_data_frame(std::uint8_t opcode,
                               const std::byte* payload,
                               std::size_t      payload_len) {
    write_data_frame_two(opcode, payload, payload_len, nullptr, 0);
}

void WsCodec::write_data_frame_two(std::uint8_t opcode,
                                   const std::byte* part1, std::size_t len1,
                                   const std::byte* part2, std::size_t len2) {
    const std::size_t total = len1 + len2;

    // Header: 2..10 bytes. Server frames MUST NOT be masked (RFC §5.1) so no
    // mask key is appended.
    std::byte hdr[10];
    std::size_t hlen = 0;
    hdr[hlen++] = static_cast<std::byte>(kBitFin | (opcode & 0x0F));
    if (total < 126) {
        hdr[hlen++] = static_cast<std::byte>(total);
    } else if (total < (1u << 16)) {
        hdr[hlen++] = static_cast<std::byte>(126);
        hdr[hlen++] = static_cast<std::byte>((total >> 8) & 0xFF);
        hdr[hlen++] = static_cast<std::byte>(total       & 0xFF);
    } else {
        hdr[hlen++] = static_cast<std::byte>(127);
        // 8-byte payload length, big-endian. Top bit MUST be 0 per RFC.
        const std::uint64_t t = static_cast<std::uint64_t>(total);
        hdr[hlen++] = static_cast<std::byte>((t >> 56) & 0xFF);
        hdr[hlen++] = static_cast<std::byte>((t >> 48) & 0xFF);
        hdr[hlen++] = static_cast<std::byte>((t >> 40) & 0xFF);
        hdr[hlen++] = static_cast<std::byte>((t >> 32) & 0xFF);
        hdr[hlen++] = static_cast<std::byte>((t >> 24) & 0xFF);
        hdr[hlen++] = static_cast<std::byte>((t >> 16) & 0xFF);
        hdr[hlen++] = static_cast<std::byte>((t >>  8) & 0xFF);
        hdr[hlen++] = static_cast<std::byte>( t        & 0xFF);
    }

    // Coalesce into one buffer to avoid tiny-packet sends (mirrors mcp_codec).
    std::vector<std::byte> frame;
    frame.reserve(hlen + total);
    frame.insert(frame.end(), hdr, hdr + hlen);
    if (len1 > 0) frame.insert(frame.end(), part1, part1 + len1);
    if (len2 > 0) frame.insert(frame.end(), part2, part2 + len2);
    send_all(frame.data(), frame.size());
}

void WsCodec::send_close(std::uint16_t status_code,
                         const std::string& reason) noexcept {
    if (close_sent_) return;
    close_sent_ = true;
    try {
        // Close payload: 2-byte status code (big-endian) + optional UTF-8 reason.
        std::vector<std::byte> payload;
        payload.reserve(2 + reason.size());
        payload.push_back(static_cast<std::byte>((status_code >> 8) & 0xFF));
        payload.push_back(static_cast<std::byte>( status_code       & 0xFF));
        for (char c : reason) {
            payload.push_back(static_cast<std::byte>(c));
        }
        if (payload.size() > kMaxControlBytes) {
            // Defensive — truncate to the 125-byte control-frame ceiling.
            payload.resize(kMaxControlBytes);
        }
        write_data_frame(kOpClose, payload.data(), payload.size());
    } catch (...) {
        // Best-effort — the peer may already be gone; silently swallow.
    }
}

void WsCodec::write_frame(const std::string& json) {
    write_data_frame(kOpBinary,
                     reinterpret_cast<const std::byte*>(json.data()),
                     json.size());
}

void WsCodec::write_frame(const std::string& json, wire::ByteView blob) {
    // RFC 6455 has no out-of-frame trailing-bytes channel — pack the JSON
    // and the blob into the same binary frame's payload. The caller's JSON
    // carries a blob_size field declaring the trailing byte count, so the
    // peer can split frame_payload[len-blob_size:] off as the blob bytes.
    write_data_frame_two(
        kOpBinary,
        reinterpret_cast<const std::byte*>(json.data()), json.size(),
        blob.data, blob.size);
}

std::optional<std::string> WsCodec::read_frame() {
    // Loop until we return a binary data frame's payload or hit EOF / close.
    while (true) {
        // Header: 2 mandatory bytes.
        std::vector<std::byte> hdr;
        if (!read_exact(2, hdr)) {
            // Clean EOF only valid at a frame boundary.
            if (buffer_.empty()) return std::nullopt;
            throw std::runtime_error("connection closed mid-WS-header");
        }
        const std::uint8_t b1 = static_cast<std::uint8_t>(hdr[0]);
        const std::uint8_t b2 = static_cast<std::uint8_t>(hdr[1]);

        const bool         fin    = (b1 & kBitFin) != 0;
        const std::uint8_t rsv    = (b1 & (kBitRsv1 | kBitRsv2 | kBitRsv3));
        const std::uint8_t opcode = b1 & 0x0F;
        const bool         masked = (b2 & kMaskFlag) != 0;
        std::uint64_t      plen   = b2 & kInitialLenMask;

        if (rsv != 0) {
            // No extensions negotiated → any RSV bit set is a protocol error.
            send_close(kCloseProtocolError, "RSV bit set");
            throw std::runtime_error("WS: reserved bit set");
        }

        const bool is_control = (opcode & 0x08) != 0;
        if (is_control && (!fin || plen > kMaxControlBytes)) {
            // RFC §5.5: control frames MUST NOT be fragmented and MUST have
            // payload ≤ 125 bytes.
            send_close(kCloseProtocolError, "control frame violation");
            throw std::runtime_error("WS: malformed control frame");
        }

        // Extended length field.
        if (plen == 126) {
            std::vector<std::byte> ext;
            if (!read_exact(2, ext)) {
                throw std::runtime_error("connection closed mid-WS-length");
            }
            plen = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(ext[0])) << 8)
                 |  static_cast<std::uint64_t>(static_cast<std::uint8_t>(ext[1]));
        } else if (plen == 127) {
            std::vector<std::byte> ext;
            if (!read_exact(8, ext)) {
                throw std::runtime_error("connection closed mid-WS-length");
            }
            plen = 0;
            for (int i = 0; i < 8; ++i) {
                plen = (plen << 8)
                     |  static_cast<std::uint64_t>(static_cast<std::uint8_t>(ext[i]));
            }
            // RFC §5.2: the top bit of the 64-bit length MUST be 0.
            if ((plen >> 63) != 0) {
                send_close(kCloseProtocolError, "length high bit set");
                throw std::runtime_error("WS: 64-bit length high bit set");
            }
        }

        if (plen > kMaxFrameBytes) {
            send_close(kCloseMessageTooBig, "frame too large");
            throw std::runtime_error("WS frame exceeds size ceiling");
        }

        // RFC §5.1: client → server frames MUST be masked. We are the server;
        // unmasked client frame is a protocol error.
        if (!masked) {
            send_close(kCloseProtocolError, "unmasked client frame");
            throw std::runtime_error("WS: unmasked client frame");
        }

        // 4-byte mask key, then `plen` payload bytes.
        std::vector<std::byte> mask_key;
        if (!read_exact(4, mask_key)) {
            throw std::runtime_error("connection closed mid-WS-mask");
        }
        std::vector<std::byte> payload;
        if (plen > 0) {
            if (!read_exact(static_cast<std::size_t>(plen), payload)) {
                throw std::runtime_error("connection closed mid-WS-payload");
            }
            // XOR-unmask in place.
            for (std::size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<std::byte>(
                    static_cast<std::uint8_t>(payload[i])
                    ^ static_cast<std::uint8_t>(mask_key[i % 4]));
            }
        }

        // Opcode dispatch.
        switch (opcode) {
            case kOpBinary: {
                if (!fin) {
                    // ARH does not accept fragmented data frames — every
                    // JSON-RPC message fits in one frame.
                    send_close(kCloseProtocolError, "fragmented frame");
                    throw std::runtime_error("WS: fragmented data frame");
                }
                std::string out(payload.size(), '\0');
                if (!payload.empty()) {
                    std::memcpy(out.data(), payload.data(), payload.size());
                }
                return out;
            }
            case kOpText: {
                // Text frames carry the same JSON in practice, but the spec
                // is binary-only — refuse so misbehaving clients are caught.
                send_close(kCloseProtocolError, "text frame not supported");
                throw std::runtime_error("WS: text frame not supported");
            }
            case kOpPing: {
                // Echo the payload back as a Pong.
                write_data_frame(kOpPong, payload.data(), payload.size());
                continue;       // keep reading for the next data frame
            }
            case kOpPong: {
                // No action required.
                continue;
            }
            case kOpClose: {
                // Per RFC 6455 §5.5.1: a close frame with a body MUST start
                // with a 2-byte status code. A 1-byte payload is malformed —
                // respond with 1002 Protocol Error rather than silently
                // normalising. Empty body is fine (no status code present).
                // Per R3 review.
                if (payload.size() == 1) {
                    send_close(kCloseProtocolError);
                    return std::nullopt;
                }
                std::uint16_t code = kCloseNormal;
                if (payload.size() >= 2) {
                    code = static_cast<std::uint16_t>(
                        (static_cast<std::uint8_t>(payload[0]) << 8)
                      |  static_cast<std::uint8_t>(payload[1]));
                }
                send_close(code);
                return std::nullopt;
            }
            case kOpContinuation: {
                // No prior data frame was sent with FIN=0, so a continuation
                // here is a protocol error.
                send_close(kCloseProtocolError, "unexpected continuation");
                throw std::runtime_error("WS: stray continuation frame");
            }
            default: {
                send_close(kCloseProtocolError, "reserved opcode");
                throw std::runtime_error("WS: reserved opcode");
            }
        }
    }
}

}  // namespace remote_hands::ws

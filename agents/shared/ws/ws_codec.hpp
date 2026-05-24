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

// RFC 6455 binary-frame codec for the `ws` framing mode (PROTOCOL.md §1.5).
//
// Wire shape (matches Mcp framing semantically; only the delimiters differ):
//
//   <FIN+opcode><MASK+initial-len>[ext-len 2 or 8][mask 4 bytes]<payload>
//
// One JSON-RPC 2.0 object per binary frame. The same shared session loop that
// drives MCP-stdio (mcp_session.cpp) drives this codec too — the only
// difference is the on-wire delimiter encoding.
//
// Residual-buffer handoff: identical to McpCodec — the bootstrap reader may
// have already pulled pipelined WS bytes off the socket while reading the
// hello header line. Construct the codec with those leftover bytes so the
// first frame (typically MCP `initialize`) is not lost.
//
// Server-side framing rules (RFC 6455):
//   - Client → server frames MUST be masked. Unmasked client frames are a
//     protocol violation; we send a Close (1002) and end the session.
//   - Server → client frames MUST NOT be masked.
//   - Fragmented data frames (FIN=0) are not used by the ARH wire protocol —
//     every JSON-RPC message fits in one frame. Reject with Close (1002).
//   - Ping (0x9): respond with Pong (0xA) carrying the same payload.
//   - Pong (0xA): no action.
//   - Close (0x8): respond with Close, then end the session gracefully.
//   - Reserved opcodes / non-RSV-zero / control frames > 125 bytes /
//     fragmented control frames: protocol violation, Close (1002).
//
// Modern-only: this TU is included in agents/windows-modern's build list.
// Legacy and classic do not link it; legacy continues to reject
// `--framing ws` with `framing_unsupported` per spec.

#include "frame_codec.hpp"
#include "../protocol.hpp"   // wire::ByteView

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>

namespace remote_hands::ws {

// RFC 6455 opcode constants (§5.2).
constexpr std::uint8_t kOpContinuation = 0x0;
constexpr std::uint8_t kOpText         = 0x1;
constexpr std::uint8_t kOpBinary       = 0x2;
constexpr std::uint8_t kOpClose        = 0x8;
constexpr std::uint8_t kOpPing         = 0x9;
constexpr std::uint8_t kOpPong         = 0xA;

// Close status codes (RFC 6455 §7.4.1).
constexpr std::uint16_t kCloseNormal           = 1000;
constexpr std::uint16_t kCloseProtocolError    = 1002;
constexpr std::uint16_t kCloseMessageTooBig    = 1009;

class WsCodec : public IFrameCodec {
public:
    // `residual` is the bootstrap wire::Reader's leftover buffer (may be
    // empty); it is consumed before any further socket reads.
    WsCodec(SOCKET socket, std::vector<std::byte> residual);

    // Reads one complete binary data frame's JSON payload. Internally handles
    // ping/pong (replies with pong) and close (replies with close, returns
    // nullopt). Returns nullopt on graceful close (peer Close or EOF after a
    // complete frame boundary). Throws std::runtime_error on a protocol
    // violation (after sending a Close frame to the peer with the appropriate
    // status code) or on an unrecoverable socket error.
    std::optional<std::string> read_frame() override;

    // Writes one unmasked binary data frame containing `json`.
    // Throws std::runtime_error on socket error.
    void write_frame(const std::string& json) override;

    // Writes one unmasked binary data frame containing `json` immediately
    // followed by exactly `blob.size` opaque bytes — i.e. one frame whose
    // payload is the concatenation of the JSON text and the raw blob bytes.
    //
    // Note vs McpCodec: the MCP overload puts the blob OUTSIDE the framed
    // message (Content-Length covers JSON only; blob bytes follow on the wire
    // between frames). RFC 6455 has no such "trailing-bytes" mechanism — the
    // frame length is authoritative — so we put both the JSON and the blob
    // bytes inside one binary frame. The wrapping JSON's `blob_size` field
    // still declares the trailing byte count, and the client reads the frame
    // payload then slices it at `len(json_bytes) == frame_len - blob_size`.
    // This matches what the spec hint "the same MCP JSON-RPC 2.0 body shape"
    // calls for at the JSON layer; the on-wire packaging is per-framing.
    void write_frame(const std::string& json, wire::ByteView blob) override;

    // Sends an unmasked close frame with the given status code. Idempotent at
    // the caller level — sending a second close on an already-closed peer
    // surfaces a recv/send error which the caller turns into session end.
    void send_close(std::uint16_t status_code,
                    const std::string& reason = {}) noexcept;

private:
    SOCKET                 socket_;
    std::vector<std::byte> buffer_;     // unconsumed bytes (residual + reads)
    bool                   close_sent_ = false;

    // Pulls more bytes from the socket into buffer_. Returns false on EOF.
    bool fill_from_socket();

    // Reads exactly `n` bytes into `out` (drawing from buffer_ first).
    // Returns false on EOF before `n` bytes are available; throws on
    // socket error.
    bool read_exact(std::size_t n, std::vector<std::byte>& out);

    // Sends `bytes` to the socket, partial-send-safe. Throws on error.
    void send_all(const std::byte* p, std::size_t left);

    // Frame assembly helpers (server-side: no mask bit, no mask key).
    void write_data_frame(std::uint8_t opcode,
                          const std::byte* payload,
                          std::size_t      payload_len);
    void write_data_frame_two(std::uint8_t opcode,
                              const std::byte* part1, std::size_t len1,
                              const std::byte* part2, std::size_t len2);
};

}  // namespace remote_hands::ws

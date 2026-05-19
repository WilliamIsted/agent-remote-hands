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

// MCP-stdio frame codec (PROTOCOL.md §1.6).
//
//   Content-Length: <N>\r\n
//   \r\n
//   <N bytes of UTF-8 JSON>
//
// One JSON-RPC 2.0 object per frame. This is the same LSP-style framing Claude
// Code uses for stdio MCP servers. The codec owns the raw socket directly once
// the bootstrap hello OK body has been consumed; the ARH wire::Reader is no
// longer driven on this connection after handoff.
//
// Residual-buffer handoff (correctness-critical): the bootstrap reader may
// have already pulled pipelined MCP bytes off the socket while reading the
// hello header line. Construct the codec with those leftover bytes so the
// first frame (typically `initialize`) is not lost.

#include "../protocol.hpp"   // wire::ByteView

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>

namespace remote_hands::mcp {

class McpCodec {
public:
    // `residual` is the wire::Reader's leftover buffer (may be empty); it is
    // consumed before any further socket reads.
    McpCodec(SOCKET socket, std::vector<std::byte> residual);

    // Reads one complete MCP frame. Returns the JSON body (the N payload
    // bytes, UTF-8) or nullopt on clean EOF. Throws std::runtime_error on a
    // malformed frame header or a mid-frame socket error / truncation.
    std::optional<std::string> read_frame();

    // Writes one MCP frame: a Content-Length header followed by `json`.
    // Throws std::runtime_error on socket error.
    void write_frame(const std::string& json);

    // Writes one Content-Length-framed JSON object followed immediately by
    // exactly `blob.size` raw opaque bytes (Protocol v3 PR1.d "Shape B"
    // binary side-channel). `Content-Length` counts ONLY the JSON object
    // bytes — framing for the JSON is identical to write_frame(json); the
    // caller is responsible for having placed a matching `blob_size` field
    // inside `json`. The blob bytes occupy the wire between this frame's
    // JSON and the next frame's `Content-Length:` header. Throws
    // std::runtime_error on socket error.
    void write_frame(const std::string& json, wire::ByteView blob);

private:
    SOCKET                 socket_;
    std::vector<std::byte> buffer_;   // unconsumed bytes (residual + reads)

    // Pulls more bytes from the socket into buffer_. Returns false on EOF.
    bool fill_from_socket();

    // Reads exactly `n` bytes (drawing from buffer_ first). Throws on
    // truncation / socket error.
    std::string read_exact(std::size_t n);

    // Extracts the next CRLF-terminated header line (CRLF stripped). Returns
    // nullopt if a full line is not yet buffered; the caller tops up.
    std::optional<std::string> try_extract_header_line();
};

}  // namespace remote_hands::mcp

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

// IFrameCodec — polymorphic frame codec interface used by the post-hello
// JSON-RPC session loop (mcp_session.cpp) to drive either:
//
//   * the MCP-stdio Content-Length codec (McpCodec, mcp_codec.cpp), or
//   * the RFC 6455 binary-frame codec (WsCodec, ws/ws_codec.cpp),
//
// without the session knowing or caring which framing is in use. The session
// owns the codec via std::unique_ptr<IFrameCodec>. McpCodec itself is not
// modified — connection.cpp wraps it in a thin McpFrameCodecAdapter that
// satisfies this interface and delegates to the underlying codec.
//
// Modern-only: WS framing is a modern-family feature; this header is included
// by the modern build's mcp_session compile path. The interface is harmless
// to expose on legacy too (only the MCP path uses it there).

#include "../protocol.hpp"   // wire::ByteView

#include <optional>
#include <string>

namespace remote_hands::ws {

class IFrameCodec {
public:
    virtual ~IFrameCodec() = default;

    // Reads one complete JSON-RPC body. Returns nullopt on clean EOF /
    // peer-initiated close. Throws std::runtime_error on framing violation
    // (the implementation is responsible for any protocol-layer close-frame
    // it needs to send before throwing) or unrecoverable socket error.
    virtual std::optional<std::string> read_frame() = 0;

    // Writes one JSON body as a framed message.
    virtual void write_frame(const std::string& json) = 0;

    // Writes one framed message whose payload is the JSON body followed by
    // `blob.size` opaque trailing bytes. The exact on-wire arrangement is
    // codec-specific:
    //
    //   * MCP: Content-Length covers JSON only; blob bytes follow the JSON
    //     on the wire (between frames).
    //   * WS:  RFC 6455 has no trailing-bytes channel; the JSON + blob bytes
    //     share a single binary frame's payload, sliced at blob_size.
    //
    // Both shapes match the same JSON-level contract: the wrapping object's
    // `blob_size` field declares the trailing count.
    virtual void write_frame(const std::string& json,
                             wire::ByteView blob) = 0;
};

}  // namespace remote_hands::ws

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

// Post-hello MCP JSON-RPC 2.0 session (PROTOCOL.md §1.6).
//
// Owns the connection from the byte immediately after the bootstrap hello OK
// body until the transport closes. Drives:
//
//   initialize                -> result (capabilities, serverInfo)
//   notifications/initialized -> consumed, no reply (it's a notification)
//   tools/list                -> result (all implemented verbs, §1.6.2/§1.6.7)
//   tools/call                -> existing verb dispatch via capture mode
//   <unknown method>          -> JSON-RPC error (-32601)
//   <malformed frame>         -> JSON-RPC parse error (-32700)
//
// Verb invocation reuses Connection::dispatch_mcp_verb() under Writer capture
// mode: the ~50 existing handlers respond into the captured buffer, the
// session reads it back and emits the MCP tools/call result. Argument mapping
// is the inverse of wire.py's _args_to_dict (§1.6.3).
//
// MODERN ONLY in Phase 1. Backed by verb_tool_meta() (verbs_blob.cpp), which
// is a windows-modern-only translation unit.

#include "json_parse.hpp"
#include "../ws/frame_codec.hpp"   // ws::IFrameCodec (codec-agnostic session)

#include <memory>
#include <string>

namespace remote_hands {
class Connection;
}

namespace remote_hands::mcp {

class McpSession {
public:
    // `codec` is owned by the session; it may be either an MCP-stdio codec
    // (Content-Length framing, §1.6) or an RFC 6455 binary-frame codec
    // (§1.5). The session loop is identical in both cases — only the
    // on-wire delimiter encoding differs, which the codec abstracts away.
    // `negotiated_version` is the ARH protocol version string echoed in
    // serverInfo.version (e.g. "2.2").
    McpSession(Connection& conn,
               std::unique_ptr<ws::IFrameCodec> codec,
               std::string negotiated_version);

    // Runs the JSON-RPC loop until the transport closes or a fatal protocol
    // error. Returns on clean EOF; throws std::runtime_error only on an
    // unrecoverable socket error (the caller's run() catches and tears down).
    void run();

private:
    Connection&                       conn_;
    std::unique_ptr<ws::IFrameCodec>  codec_;
    std::string                       negotiated_version_;
    bool              initialized_ = false;

    // Frame handlers. `id_json` is the verbatim JSON text of the request's
    // `id` member (number or string) so the reply echoes it byte-faithfully;
    // empty means the inbound frame was a notification (no reply).
    void handle_initialize(const std::string& id_json, const JsonValue& msg);
    void handle_tools_list(const std::string& id_json);
    void handle_tools_call(const std::string& id_json, const JsonValue& msg);

    void send_result(const std::string& id_json, const std::string& result_obj);
    void send_error(const std::string& id_json, int code,
                    const std::string& message);
};

}  // namespace remote_hands::mcp

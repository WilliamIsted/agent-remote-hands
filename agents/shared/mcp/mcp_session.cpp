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

#include "mcp_session.hpp"

#include "json_parse.hpp"
#include "../capabilities.hpp"
#include "../connection.hpp"
#include "../errors.hpp"
#include "../log.hpp"
#include "../protocol.hpp"
#include "../verbs_blob.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace remote_hands::mcp {

namespace {

// JSON string escaper (RFC 8259). Mirrors json.hpp's append_string but lives
// here so the builder-only json.hpp is not coupled to the MCP module.
void append_json_string(std::string& out, std::string_view s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char esc[8];
                    std::snprintf(esc, sizeof(esc), "\\u%04x",
                                  static_cast<unsigned>(
                                      static_cast<unsigned char>(c)));
                    out += esc;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

// Re-serialise a parsed JSON value as its compact JSON text. Used to echo the
// request `id` faithfully (numbers keep their exact lexeme; strings are
// re-escaped). Only the value kinds a JSON-RPC `id` can take are needed
// (string / number / null) but objects/arrays are handled for completeness.
std::string serialise(const JsonValue& v) {
    switch (v.type()) {
        case JsonType::Null:   return "null";
        case JsonType::Bool:   return v.as_bool() ? "true" : "false";
        case JsonType::Number: return v.num_lexeme();
        case JsonType::String: {
            std::string s;
            append_json_string(s, v.as_string());
            return s;
        }
        case JsonType::Array: {
            std::string s = "[";
            bool first = true;
            for (const auto& e : v.as_array()) {
                if (!first) s += ',';
                first = false;
                s += serialise(e);
            }
            s += ']';
            return s;
        }
        case JsonType::Object: {
            std::string s = "{";
            bool first = true;
            for (const auto& kv : v.as_object()) {
                if (!first) s += ',';
                first = false;
                append_json_string(s, kv.first);
                s += ':';
                s += serialise(kv.second);
            }
            s += '}';
            return s;
        }
    }
    return "null";
}

// Best-effort positional flattening — the inverse of wire.py _args_to_dict
// (§1.6.3). Reconstructs the flat token stream the existing (not-yet-migrated)
// verb handlers expect:
//
//   "_args": [a, b]   -> leading positional tokens a b (array order)
//   "flag": true      -> --flag           (bare; no value token)
//   "key": <scalar>   -> --key <value>    ('_'->'-' in the key)
//
// Scalars: string verbatim; number as its JSON lexeme; bool false -> "false".
//
// Phase 2.0 change: this NO LONGER hard-rejects. Nested object/array values
// and `content_b64` simply cannot be represented as flat tokens, so this skips
// them in the positional projection — the named view (req.named_root,
// attached separately by the caller) carries them losslessly for handlers
// migrated in Phase 2.1. A handler still on the positional path that needs a
// skipped arg will fail with its OWN invalid_args, which is the accepted
// transitional behaviour (Phase 2.1 migrates exactly those handlers). Nothing
// that flattened cleanly in Phase 1 flattens differently now, so no Phase-1
// positional path regresses. `void` return: there is no longer a reject state.
void map_arguments(const JsonValue& args, wire::Request& req) {
    if (args.is_null() || !args.is_object()) {
        // No arguments (e.g. system.info) or a non-object value. The named
        // view (if any) is attached by the caller regardless; the positional
        // projection is simply empty here.
        return;
    }

    // Pass 1: positional _args (array order) become leading tokens. A
    // non-array _args or a non-scalar entry can't be projected positionally —
    // skip it rather than reject; the named view still carries it verbatim.
    if (const JsonValue* pos = args.find("_args")) {
        if (pos->is_array()) {
            for (const auto& e : pos->as_array()) {
                if (e.is_string()) {
                    req.args.push_back(e.as_string());
                } else if (e.is_number()) {
                    req.args.push_back(e.num_lexeme());
                } else if (e.is_bool()) {
                    req.args.push_back(e.as_bool() ? "true" : "false");
                }
                // Non-scalar _args entry: skip in the positional projection.
            }
        }
    }

    // Pass 2: every other key becomes --key [value]. Keys whose value is a
    // nested object/array, or the binary side-channel `content_b64`, cannot
    // be flattened to the token grammar — skip them here (the named view holds
    // them). This is the strangler seam: positional stays best-effort while
    // handlers migrate.
    for (const auto& kv : args.as_object()) {
        const std::string& key = kv.first;
        if (key == "_args") continue;
        if (key == "content_b64") continue;   // binary side-channel: named-only

        const JsonValue& val = kv.second;
        if (val.is_object() || val.is_array()) {
            // Nested shape (region:{x,y,w,h}, modifiers:[...], argv:[...], …):
            // not representable positionally. Carried by the named view only.
            continue;
        }

        std::string flag = "--";
        for (char c : key) flag += (c == '_') ? '-' : c;
        req.args.push_back(flag);

        if (val.is_bool()) {
            if (val.as_bool()) {
                // Bare flag — no value token (matches _args_to_dict's
                // `--flag` alone => {"flag": true}).
                continue;
            }
            req.args.push_back("false");
        } else if (val.is_string()) {
            req.args.push_back(val.as_string());
        } else if (val.is_number()) {
            req.args.push_back(val.num_lexeme());
        } else if (val.is_null()) {
            // Null value: drop the flag we just pushed (treat as absent —
            // matches the named view's has()/null == absent semantics).
            req.args.pop_back();
        }
    }
}

}  // namespace

McpSession::McpSession(Connection& conn,
                       McpCodec codec,
                       std::string negotiated_version)
    : conn_{conn},
      codec_{std::move(codec)},
      negotiated_version_{std::move(negotiated_version)} {}

void McpSession::send_result(const std::string& id_json,
                             const std::string& result_obj) {
    std::string frame = "{\"jsonrpc\":\"2.0\",\"id\":";
    frame += id_json;
    frame += ",\"result\":";
    frame += result_obj;
    frame += '}';
    codec_.write_frame(frame);
}

void McpSession::send_error(const std::string& id_json, int code,
                            const std::string& message) {
    std::string frame = "{\"jsonrpc\":\"2.0\",\"id\":";
    frame += id_json.empty() ? "null" : id_json;
    frame += ",\"error\":{\"code\":";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", code);
    frame += buf;
    frame += ",\"message\":";
    append_json_string(frame, message);
    frame += "}}";
    codec_.write_frame(frame);
}

void McpSession::handle_initialize(const std::string& id_json,
                                   const JsonValue& /*msg*/) {
    // Advertise tools + notifications; serverInfo.version echoes the
    // negotiated ARH protocol version (§1.6.1).
    std::string result =
        "{\"protocolVersion\":\"2025-03-26\","
        "\"capabilities\":{\"tools\":{\"listChanged\":false},"
        "\"notifications\":{}},"
        "\"serverInfo\":{\"name\":\"AgentRemoteHands\",\"version\":";
    append_json_string(result, negotiated_version_);
    result += "}}";
    send_result(id_json, result);
    initialized_ = true;
}

void McpSession::handle_tools_list(const std::string& id_json) {
    // §1.6.2: ALL implemented verbs regardless of tier; each carries its
    // pre-sliced description / inputSchema / x-crudx / x-tier fragment.
    // §1.6.7 excludes are already absent from verb_tool_meta(). We further
    // gate on find_verb() so connection-tier verbs (handled outside the verb
    // table) and any family-disabled verbs are reflected accurately.
    const auto& meta = system_verbs::verb_tool_meta();

    std::string result = "{\"tools\":[";
    bool first = true;
    for (const auto& kv : meta) {
        const std::string_view name = kv.first;
        // connection.tier_raise / connection.tier_drop are MCP tools (§1.6.4)
        // but are NOT in the verb table (handled by Connection directly), so
        // find_verb() would reject them. Allow them through explicitly;
        // everything else must be a live, family-enabled verb.
        const bool is_tier_verb =
            (name == "connection.tier_raise" ||
             name == "connection.tier_drop");
        if (!is_tier_verb && !find_verb(name)) {
            continue;
        }
        if (!first) result += ',';
        first = false;
        result += "{\"name\":";
        append_json_string(result, name);
        result += kv.second;   // ,"description":...,"inputSchema":...,x-*
        result += '}';
    }
    result += "]}";
    send_result(id_json, result);
}

void McpSession::handle_tools_call(const std::string& id_json,
                                   const JsonValue& msg) {
    const JsonValue* params = msg.find("params");
    if (!params || !params->is_object()) {
        send_error(id_json, -32602, "tools/call missing params object");
        return;
    }
    const JsonValue* name = params->find("name");
    if (!name || !name->is_string() || name->as_string().empty()) {
        send_error(id_json, -32602, "tools/call missing tool name");
        return;
    }
    const std::string& verb = name->as_string();

    // §1.6.7: these are never MCP tools. Naming one is a protocol-level
    // "unknown tool" condition, not a verb-level failure.
    if (verb == "connection.hello" || verb == "connection.close" ||
        verb == "connection.reset" || verb == "system.verbs") {
        send_error(id_json, -32601,
                   "tool '" + verb + "' is not exposed over MCP");
        return;
    }

    // Phase 2.0 (strangler-fig): populate BOTH the named view and the
    // positional projection on `req`.
    //
    //  * named_root  — the parsed `params.arguments` object, owned for the
    //                  request's lifetime. Handlers migrated in Phase 2.1 read
    //                  it via agents/shared/verbs/args.hpp. Carries nested /
    //                  array / binary shapes losslessly.
    //  * args        — best-effort positional flattening (unchanged behaviour
    //                  for everything that flattened in Phase 1; nested/array/
    //                  binary keys are now skipped instead of hard-rejecting).
    //
    // Previously-rejected nested/array (and content_b64) requests now reach
    // the handler. A handler still on the positional path may then fail with
    // its own invalid_args — that is the accepted transitional state; Phase
    // 2.1 migrates exactly those handlers to the named view. No Phase-1
    // positional path changes shape, so nothing that worked regresses.
    wire::Request req;
    req.verb = verb;
    const JsonValue* arguments = params->find("arguments");
    if (arguments && !arguments->is_null()) {
        // Own a copy: `arguments` is a sub-node of the per-frame parse tree
        // that goes out of scope when this function returns. dispatch_mcp_verb
        // runs synchronously below so a bounded lifetime would suffice, but an
        // owned shared_ptr is the contract protocol.hpp declares and keeps the
        // named view valid for the whole request without lifetime coupling to
        // the codec frame.
        req.named_root = std::make_shared<const JsonValue>(*arguments);
    }
    JsonValue empty_obj = JsonValue::make_object({});
    map_arguments(arguments ? *arguments : empty_obj, req);

    // Dispatch through the shared connection/verb policy in capture mode.
    auto& writer = conn_.writer();
    writer.begin_capture();
    conn_.dispatch_mcp_verb(req);
    const wire::Writer::Captured cap = writer.captured();
    writer.end_capture();

    if (!cap.responded) {
        // A handler that returned without writing — should not happen; treat
        // as an internal verb error rather than hang the client.
        std::string result =
            "{\"content\":[{\"type\":\"text\",\"text\":"
            "\"{\\\"message\\\":\\\"verb produced no response\\\"}\"}],"
            "\"isError\":true,\"arh_error_code\":\"not_supported\"}";
        send_result(id_json, result);
        return;
    }

    if (cap.has_blob) {
        // Binary side-channel — Protocol v3 PR1.d "Shape B". The JSON-RPC
        // result object IS the verb's metadata plus a blob_size declaring how
        // many raw opaque bytes follow the JSON's last byte (before the next
        // frame's Content-Length). Content-Length counts ONLY the JSON. This
        // path is reached exclusively by write_ok_blob() (screen.capture
        // encoding:binary, modern family); every other response keeps the
        // text-content path below byte-for-byte unchanged.
        std::string frame = "{\"jsonrpc\":\"2.0\",\"id\":";
        frame += id_json;
        frame += ",\"result\":{";
        if (!cap.blob_meta.empty()) {
            frame += cap.blob_meta;   // pre-formatted JSON members, no braces
            frame += ',';
        }
        frame += "\"blob_size\":";
        char nbuf[32];
        std::snprintf(nbuf, sizeof(nbuf), "%zu", cap.blob.size());
        frame += nbuf;
        frame += "}}";
        codec_.write_frame(
            frame,
            wire::ByteView{
                reinterpret_cast<const std::byte*>(cap.blob.data()),
                cap.blob.size()});
        return;
    }

    std::string result = "{\"content\":[{\"type\":\"text\",\"text\":";
    if (cap.is_err) {
        const std::string detail =
            cap.err_detail.empty() ? std::string("{}") : cap.err_detail;
        append_json_string(result, detail);
        result += "}],\"isError\":true,\"arh_error_code\":";
        append_json_string(result, to_wire(cap.err_code));
        result += '}';
    } else {
        // OK body is the verb's response JSON (may be empty for bodyless OK;
        // wire.py treats empty text as an empty payload, matching v2.1).
        append_json_string(result, cap.ok_body);
        result += "}],\"isError\":false}";
    }
    send_result(id_json, result);
}

void McpSession::run() {
    while (true) {
        std::optional<std::string> raw;
        try {
            raw = codec_.read_frame();
        } catch (const std::exception& ex) {
            // Malformed framing / truncation: log and end the session. The
            // socket state is unknown so we cannot reliably reply.
            log::warning(L"MCP framing error: %hs", ex.what());
            return;
        }
        if (!raw.has_value()) {
            // Clean EOF — client closed the transport (the MCP analogue of
            // connection.close, §1.6.7).
            return;
        }

        auto parsed = parse(*raw);
        if (!parsed.has_value() || !parsed->is_object()) {
            // JSON-RPC parse error. No reliable id — reply with null id.
            send_error("", -32700, "parse error");
            continue;
        }
        const JsonValue& msg = *parsed;

        const JsonValue* method_v = msg.find("method");
        const JsonValue* id_v     = msg.find("id");
        const bool is_notification = (id_v == nullptr);
        const std::string id_json = id_v ? serialise(*id_v) : std::string{};

        if (!method_v || !method_v->is_string()) {
            if (!is_notification) {
                send_error(id_json, -32600, "invalid request: no method");
            }
            continue;
        }
        const std::string& method = method_v->as_string();

        if (method == "initialize") {
            handle_initialize(id_json, msg);
        } else if (method == "notifications/initialized") {
            // Notification — MUST NOT reply (§1.6.1 step 3).
            initialized_ = true;
        } else if (method == "tools/list") {
            handle_tools_list(id_json);
        } else if (method == "tools/call") {
            handle_tools_call(id_json, msg);
        } else if (method == "ping") {
            // MCP utility ping — empty result. (Harmless to support; some
            // clients send it. Notifications get no reply.)
            if (!is_notification) send_result(id_json, "{}");
        } else {
            if (!is_notification) {
                send_error(id_json, -32601,
                           "method not found: " + method);
            }
            // Unknown notifications are silently ignored per JSON-RPC.
        }
    }
}

}  // namespace remote_hands::mcp

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

#include "connection.hpp"

#include "capabilities.hpp"
#include "element_table.hpp"
#include "json.hpp"
#include "log.hpp"
#include "subscription.hpp"
#include "sysinfo.hpp"

#ifdef RH_MCP
#include "platform.hpp"
#include "mcp/mcp_codec.hpp"
#include "mcp/mcp_session.hpp"
#endif

#include <stdexcept>
#include <string>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>

namespace remote_hands {

namespace {

// Minimal JSON-detail formatter helpers. Builds short strings without
// pulling in a full JSON library. Caller is responsible for ensuring no
// embedded `"` or `\` characters in the values passed in.
std::string json_kv(std::string_view k1, std::string_view v1) {
    std::string s;
    s.reserve(k1.size() + v1.size() + 8);
    s += '{'; s += '"'; s.append(k1); s += "\":\""; s.append(v1); s += "\"}";
    return s;
}

std::string json_kv2(std::string_view k1, std::string_view v1,
                     std::string_view k2, std::string_view v2) {
    std::string s;
    s += '{';
    s += '"'; s.append(k1); s += "\":\""; s.append(v1); s += '"';
    s += ',';
    s += '"'; s.append(k2); s += "\":\""; s.append(v2); s += '"';
    s += '}';
    return s;
}

}  // namespace

Connection::Connection(SOCKET socket,
                       std::shared_ptr<const TokenStore> token_store,
                       int max_connections)
    : socket_{socket},
      token_store_{std::move(token_store)},
      max_connections_{max_connections},
      reader_{socket},
      writer_{socket},
      element_table_{std::make_unique<ElementTable>()},
      subscriptions_{std::make_unique<SubscriptionRegistry>()} {}

Connection::~Connection() {
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
    }
}

void Connection::run() {
    log::debug(L"Connection thread started");
    try {
        while (state_ != State::Closed) {
            auto maybe_req = reader_.read_header();
            if (!maybe_req.has_value()) {
                log::debug(L"Connection closed by peer");
                break;
            }
            try {
                dispatch(*maybe_req);
            } catch (const std::exception& ex) {
                log::warning(L"Verb dispatch failed: %hs", ex.what());
                writer_.write_err(ErrorCode::InvalidArgs,
                                  json_kv("message", ex.what()));
            }

#ifdef RH_MCP
            // v2.2 framing handoff (§1.6). handle_hello() set this once the
            // hello OK body has been written (ARH-framed) to the socket. From
            // the next byte the connection speaks MCP-stdio. The bootstrap
            // Reader may already hold pipelined MCP bytes (the client's
            // `initialize` frame) — take_residual() moves them into the codec
            // so they are not lost. The MCP session owns the socket until the
            // transport closes; on return we fall through to the existing
            // subscription teardown so cleanup is identical to the ARH path.
            if (switch_to_mcp_) {
                log::debug(L"Switching connection to MCP framing");
                mcp::McpCodec codec(reader_.socket(), reader_.take_residual());
                mcp::McpSession session(*this, std::move(codec),
                                        negotiated_protocol_);
                session.run();
                break;
            }
#endif
        }
    } catch (const std::exception& ex) {
        log::warning(L"Connection terminated: %hs", ex.what());
    }

    // Cancel subscriptions BEFORE the destructor runs. Without this, an
    // exception path (recv() failure on a peer that closed abruptly, etc.)
    // tears down the Connection while subscription threads are still
    // holding pointers into our writer_ / element_table_ — use-after-free.
    // The graceful connection.close path also calls cancel_all() in its
    // handler; doing it here too is idempotent and covers the exception
    // path. Closes #61.
    if (subscriptions_) {
        subscriptions_->cancel_all();
    }
    state_ = State::Closed;
}

void Connection::dispatch(const wire::Request& req) {
#ifdef RH_DEBUG
    {
        std::string line = req.verb;
        for (const auto& a : req.args) { line += ' '; line += a; }
        log::debug(L">> %hs", line.c_str());
    }
#endif
    // Header tokenisation failed (e.g. unmatched quote per PROTOCOL.md
    // §1.2.5). Surface as ERR invalid_args rather than dispatching.
    if (!req.parse_error.empty()) {
        writer_.write_err(ErrorCode::InvalidArgs,
                          json_kv("message", req.parse_error));
        return;
    }

    // Pre-hello state restricts the verb surface.
    if (state_ == State::PreHello) {
        if (req.verb == "connection.hello") {
            handle_hello(req);
            return;
        }
        if (req.verb == "connection.close") {
            handle_close(req);
            return;
        }
        writer_.write_err(ErrorCode::InvalidState,
                          json_kv("required", "hello"));
        return;
    }

    // connection.* verbs are tier-agnostic.
    if (req.verb == "connection.hello") {
        // A second hello is invalid; we're already past pre-hello.
        writer_.write_err(ErrorCode::InvalidState,
                          json_kv("message", "already hello'd"));
        return;
    }
    if (req.verb == "connection.tier_raise") { handle_tier_raise(req); return; }
    if (req.verb == "connection.tier_drop")  { handle_tier_drop(req);  return; }
    if (req.verb == "connection.reset")      { handle_reset(req);      return; }
    if (req.verb == "connection.close")      { handle_close(req);      return; }

    // Verb table lookup (system.*, screen.*, etc.).
    if (const auto* entry = find_verb(req.verb)) {
        if (!tier_satisfies(entry->required_tier, tier_)) {
            writer_.write_err(
                ErrorCode::TierRequired,
                json_kv2("required", to_wire(entry->required_tier),
                         "current",  to_wire(tier_)));
            return;
        }
        entry->handler(*this, req);
        return;
    }

    // Verb genuinely not implemented in this build.
    handle_unimplemented(req);
}

void Connection::dispatch_mcp_verb(const wire::Request& req) {
    // Mirrors the post-hello portion of dispatch(), minus the verbs §1.6.7
    // excludes from MCP. The caller (mcp_session) has already put writer()
    // in capture mode; every path below records into the captured response.

    if (!req.parse_error.empty()) {
        writer_.write_err(ErrorCode::InvalidArgs,
                          json_kv("message", req.parse_error));
        return;
    }

    // Tier transitions are exposed as MCP tools (§1.6.4). They are not in the
    // verb table — route to the dedicated handlers, exactly as dispatch() does.
    //
    // The ARH bootstrap form is positional (`connection.tier_raise <tier>
    // <token>`); the MCP arguments object is named per the spec input_schema
    // (`{tier, token}`), so the inverse arg map produces flag tokens
    // (`--tier <v> --token <v>`). Re-flatten to the positional shape the
    // shared handlers parse — these args are flat scalars (fully Phase-1
    // supported), this is purely a named↔positional bridge for the two
    // connection-tier verbs whose schema is known and fixed.
    if (req.verb == "connection.tier_raise" ||
        req.verb == "connection.tier_drop") {
        wire::Request adapted;
        adapted.verb = req.verb;
        std::string tier_val, token_val;
        bool have_tier = false, have_token = false;
        for (std::size_t i = 0; i < req.args.size(); ++i) {
            const std::string& a = req.args[i];
            if (a == "--tier" && i + 1 < req.args.size()) {
                tier_val = req.args[++i]; have_tier = true;
            } else if (a == "--token" && i + 1 < req.args.size()) {
                token_val = req.args[++i]; have_token = true;
            } else if (a.size() >= 2 && a.compare(0, 2, "--") == 0) {
                // Unknown flag — let the handler's own arg-count check fire
                // a clean invalid_args by passing args through unmodified.
                have_tier = false;
                break;
            } else if (!have_tier) {
                // Tolerate a positional form too (defensive).
                tier_val = a; have_tier = true;
            } else if (!have_token) {
                token_val = a; have_token = true;
            }
        }
        if (have_tier) {
            adapted.args.push_back(tier_val);
            if (have_token) adapted.args.push_back(token_val);
            if (req.verb == "connection.tier_raise") handle_tier_raise(adapted);
            else                                     handle_tier_drop(adapted);
            return;
        }
        // Fall through with the original request so the handler emits its
        // canonical invalid_args message.
        if (req.verb == "connection.tier_raise") handle_tier_raise(req);
        else                                     handle_tier_drop(req);
        return;
    }

    // §1.6.7: these are never surfaced as MCP tools. tools/list excludes them
    // and a tools/call naming them is an unknown tool — mcp_session maps that
    // to an MCP protocol-level error before reaching here. Defensive guard.
    if (req.verb == "connection.hello" ||
        req.verb == "connection.close" ||
        req.verb == "connection.reset" ||
        req.verb == "system.verbs") {
        writer_.write_err(ErrorCode::NotSupported,
                          json_kv("message", "verb not available over MCP"));
        return;
    }

    if (const auto* entry = find_verb(req.verb)) {
        if (!tier_satisfies(entry->required_tier, tier_)) {
            writer_.write_err(
                ErrorCode::TierRequired,
                json_kv2("required", to_wire(entry->required_tier),
                         "current",  to_wire(tier_)));
            return;
        }
        entry->handler(*this, req);
        return;
    }

    handle_unimplemented(req);
}

// ---------------------------------------------------------------------------
// connection.* handlers

#ifdef RH_MCP

namespace {

// Parse "<major>.<minor>" leniently. Returns false if not "2.<n>" / "2".
bool parse_v2_minor(const std::string& v, int& minor_out) {
    if (v == "2") { minor_out = 0; return true; }
    if (v.rfind("2.", 0) != 0) return false;
    const std::string rest = v.substr(2);
    if (rest.empty()) return false;
    int m = 0;
    for (char c : rest) {
        if (c == '.') break;          // ignore patch component
        if (c < '0' || c > '9') return false;
        if (m > 100000) { minor_out = m; return true; }
        m = m * 10 + (c - '0');
    }
    minor_out = m;
    return true;
}

// 128-bit hex session id (opaque per-connection correlation id). Reuses the
// agent's existing CSPRNG seam (platform::generate_random_bytes), the same
// source token.cpp uses — no new entropy primitive introduced.
std::string make_session_id() {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    try {
        const auto raw = platform::generate_random_bytes(16);
        out.reserve(raw.size() * 2);
        for (auto b : raw) {
            out.push_back(kHex[(b >> 4) & 0x0f]);
            out.push_back(kHex[b & 0x0f]);
        }
    } catch (...) {
        out = "sess-unavailable";
    }
    return out;
}

}  // namespace

void Connection::handle_hello(const wire::Request& req) {
    // v2.2 bootstrap (PROTOCOL.md §1.2):
    //   connection.hello <client-name> <client-version> [--framing mcp|ws]
    // Bootstrap framing for the hello exchange itself stays ARH header-line;
    // the negotiated framing takes over only after the OK body is consumed.
    if (req.args.size() < 2) {
        writer_.write_err(ErrorCode::InvalidArgs,
                          json_kv("message",
                                  "connection.hello requires <client-name> <client-version>"));
        return;
    }
    const std::string& client_name = req.args[0];
    const std::string& version     = req.args[1];

    // Optional --framing mcp|ws (defaults to mcp on a v2.2 session).
    std::string framing = "mcp";
    for (std::size_t i = 2; i < req.args.size(); ++i) {
        if (req.args[i] == "--framing" && i + 1 < req.args.size()) {
            framing = req.args[++i];
        } else if (req.args[i].size() >= 2 &&
                   req.args[i].compare(0, 2, "--") == 0) {
            writer_.write_err(ErrorCode::InvalidArgs,
                              json_kv("unknown_flag", req.args[i]));
            return;
        }
    }

    // v2.2+ agents do NOT advertise v2.1: require negotiated minor >= 2.
    int minor = 0;
    if (!parse_v2_minor(version, minor) || minor < 2) {
        writer_.write_err(
            ErrorCode::ProtocolMismatch,
            json_kv2("agent", "2.2", "client", version));
        return;
    }
    negotiated_protocol_ = "2.2";

    if (framing == "ws") {
        // WS framing (§1.5) is windows-modern-only and not implemented yet
        // (a later phase). windows-legacy never supports ws regardless. Both
        // reject explicitly (ARH-framed) so the client does not switch its
        // parser. Empty-detail ERR per §1.2.
        writer_.write_err(ErrorCode::FramingUnsupported);
        state_ = State::Closed;
        return;
    }
    if (framing != "mcp") {
        writer_.write_err(ErrorCode::FramingUnsupported);
        return;
    }

    // 7-field hello OK body (connection.hello.json x-output-schema;
    // additionalProperties:false, all 7 required).
    std::string body;
    body += '{';
    json::append_kv_string(body, "protocol", "arh");                 body += ',';
    json::append_kv_string(body, "agent", "AgentRemoteHands");       body += ',';
    json::append_kv_string(body, "agent_protocol", negotiated_protocol_); body += ',';
    json::append_kv_string(body, "os_name", sysinfo::os_name());     body += ',';
    json::append_kv_string(body, "os_version", sysinfo::os_version()); body += ',';
    json::append_kv_string(body, "session_id", make_session_id());   body += ',';
    json::append_kv_string(body, "framing", "mcp");
    body += '}';

    state_         = State::Connected;
    switch_to_mcp_ = true;     // run() performs the handoff after the OK body
    log::info(L"Hello from %hs (protocol %hs, framing mcp)",
              client_name.c_str(), version.c_str());
    writer_.write_ok(body);    // still ARH-framed (the hello response itself)
}

#else  // !RH_MCP  — classic (C++ non-MCP build) keeps the v2.1 header-line
       // behaviour unchanged. As of Phase 2.0 legacy DOES define RH_MCP and
       // takes the v2.2 MCP path above; only windows-classic stays here.

void Connection::handle_hello(const wire::Request& req) {
    // connection.hello <client-name> <protocol-version>
    if (req.args.size() != 2) {
        writer_.write_err(ErrorCode::InvalidArgs,
                          json_kv("message",
                                  "connection.hello requires <client-name> <protocol-version>"));
        return;
    }
    const std::string& version = req.args[1];

    // Major-version compare. We accept any "2.x".
    if (version.rfind("2.", 0) != 0 && version != "2") {
        writer_.write_err(
            ErrorCode::ProtocolMismatch,
            json_kv2("agent", "2", "client", version));
        return;
    }

    state_ = State::Connected;
    log::info(L"Hello from %hs (protocol %hs)",
              req.args[0].c_str(), version.c_str());
    writer_.write_ok();
}

#endif  // RH_MCP

void Connection::handle_tier_raise(const wire::Request& req) {
    // connection.tier_raise <tier> <token>
    if (req.args.size() != 2) {
        writer_.write_err(ErrorCode::InvalidArgs,
                          json_kv("message",
                                  "connection.tier_raise requires <tier> <token>"));
        return;
    }
    const auto requested = tier_from_wire(req.args[0]);
    if (!requested.has_value()) {
        writer_.write_err(ErrorCode::InvalidArgs,
                          json_kv("message", "unknown tier"));
        return;
    }
    if (!tier_satisfies(*requested, *requested)) {
        // unreachable; placate compiler.
    }
    if (static_cast<int>(*requested) <= static_cast<int>(tier_)) {
        // Cannot raise to a tier we already have or are above.
        writer_.write_err(ErrorCode::InvalidArgs,
                          json_kv2("message", "use tier_drop for downgrades",
                                   "current", to_wire(tier_)));
        return;
    }
    if (!token_store_ || !token_store_->verify(req.args[1])) {
        writer_.write_err(ErrorCode::AuthInvalid,
                          json_kv("message", "token mismatch"));
        return;
    }

    tier_ = *requested;
    log::info(L"Tier raised to %hs", to_wire(tier_).data());
    writer_.write_ok(json_kv("new_tier", std::string{to_wire(tier_)}));
}

void Connection::handle_tier_drop(const wire::Request& req) {
    // connection.tier_drop <tier>
    if (req.args.size() != 1) {
        writer_.write_err(ErrorCode::InvalidArgs,
                          json_kv("message",
                                  "connection.tier_drop requires <tier>"));
        return;
    }
    const auto requested = tier_from_wire(req.args[0]);
    if (!requested.has_value()) {
        writer_.write_err(ErrorCode::InvalidArgs,
                          json_kv("message", "unknown tier"));
        return;
    }
    if (static_cast<int>(*requested) > static_cast<int>(tier_)) {
        writer_.write_err(ErrorCode::InvalidArgs,
                          json_kv2("message", "use tier_raise for upgrades",
                                   "current", to_wire(tier_)));
        return;
    }

    tier_ = *requested;
    log::info(L"Tier dropped to %hs", to_wire(tier_).data());
    writer_.write_ok(json_kv("new_tier", std::string{to_wire(tier_)}));
}

void Connection::handle_reset(const wire::Request& /*req*/) {
    reader_.flush_buffer();
    writer_.write_ok();
}

void Connection::handle_close(const wire::Request& /*req*/) {
    // PROTOCOL.md §2.5: drain pending EVENT frames before responding.
    // cancel_all() stops every subscription thread and joins them, so any
    // in-flight EVENT writes have completed by the time it returns.
    subscriptions_->cancel_all();
    writer_.write_ok();
    state_ = State::Closed;
}

void Connection::handle_unimplemented(const wire::Request& req) {
    log::debug(L"Verb '%hs' not yet implemented", req.verb.c_str());
    writer_.write_err(
        ErrorCode::NotSupported,
        json_kv2("verb", req.verb, "reason", "not yet implemented in this build"));
}

}  // namespace remote_hands

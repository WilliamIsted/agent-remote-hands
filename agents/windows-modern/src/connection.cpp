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
#include "log.hpp"
#include "subscription.hpp"

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
        }
    } catch (const std::exception& ex) {
        log::warning(L"Connection terminated: %hs", ex.what());
    }
    state_ = State::Closed;
}

void Connection::dispatch(const wire::Request& req) {
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

// ---------------------------------------------------------------------------
// connection.* handlers

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

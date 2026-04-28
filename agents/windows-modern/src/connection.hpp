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

// Per-connection state machine and verb dispatch.
//
// One Connection instance per accepted TCP socket. The state machine follows
// PROTOCOL.md §2.1:
//
//   pre_hello  -> connected (after connection.hello)
//   connected  -> connected (every other verb; tier transitions internally)
//   connected  -> closed    (on connection.close, socket drop, or fatal error)
//
// run() blocks the calling thread for the connection's lifetime. The Server
// spawns one thread per accepted connection and parks each in run().

#include "protocol.hpp"
#include "tier.hpp"
#include "token.hpp"

#include <memory>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>

namespace remote_hands {

class ElementTable;            // see element_table.hpp
class SubscriptionRegistry;    // see subscription.hpp

class Connection {
public:
    Connection(SOCKET socket,
               std::shared_ptr<const TokenStore> token_store,
               int max_connections);
    ~Connection();
    Connection(const Connection&)            = delete;
    Connection& operator=(const Connection&) = delete;

    // Drives the state machine until close. Closes the socket on return.
    void run();

    // Public accessors for verb handlers (declared in capabilities.cpp /
    // implemented in verbs/<namespace>.cpp). Verbs read/write the wire via
    // these and consult tier() if they need it.
    wire::Reader&           reader() noexcept { return reader_; }
    wire::Writer&           writer() noexcept { return writer_; }
    Tier                    tier()   const noexcept { return tier_; }
    int                     max_connections() const noexcept { return max_connections_; }
    ElementTable&           element_table() noexcept { return *element_table_; }
    SubscriptionRegistry&   subscriptions() noexcept { return *subscriptions_; }

    // Crash-detection focus tracking (see crash_check.hpp).
    // window.focus calls note_focus_target() on success. Input verbs
    // consult focus_track_*() through crash_check::check_focus_or_fail().
    void note_focus_target(HWND hwnd, DWORD pid) noexcept {
        focus_track_hwnd_ = hwnd;
        focus_track_pid_  = pid;
    }
    HWND  focus_track_hwnd() const noexcept { return focus_track_hwnd_; }
    DWORD focus_track_pid()  const noexcept { return focus_track_pid_; }

private:
    enum class State {
        PreHello,
        Connected,
        Closed,
    };

    void dispatch(const wire::Request& req);

    // connection.* verb handlers (the only verbs accepted in PreHello).
    void handle_hello(const wire::Request& req);
    void handle_tier_raise(const wire::Request& req);
    void handle_tier_drop(const wire::Request& req);
    void handle_reset(const wire::Request& req);
    void handle_close(const wire::Request& req);

    // Default handler for verbs not yet implemented (filled in later phases).
    void handle_unimplemented(const wire::Request& req);

    SOCKET                              socket_;
    std::shared_ptr<const TokenStore>   token_store_;
    int                                 max_connections_;

    wire::Reader    reader_;
    wire::Writer    writer_;

    State           state_  = State::PreHello;
    Tier            tier_   = Tier::Observe;

    std::unique_ptr<ElementTable>          element_table_;
    std::unique_ptr<SubscriptionRegistry>  subscriptions_;

    HWND   focus_track_hwnd_ = nullptr;
    DWORD  focus_track_pid_  = 0;
};

}  // namespace remote_hands

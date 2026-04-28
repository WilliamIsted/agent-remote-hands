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

// Streaming subscription infrastructure.
//
// `watch.*` verbs each register a Subscription instance with the connection's
// SubscriptionRegistry. Each Subscription owns one background thread that
// emits `EVENT <sub-id> <length>\n<bytes>` frames to the connection's
// thread-safe Writer. On `watch.cancel` (or connection close) the
// Subscription is signalled to stop, joined, and removed from the registry.

#include "protocol.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace remote_hands {

// Abstract base. Subclasses implement run() with their watch-specific logic
// and call emit() to dispatch EVENT frames.
class Subscription {
public:
    Subscription(wire::Writer& writer, std::string id);
    virtual ~Subscription();

    Subscription(const Subscription&)            = delete;
    Subscription& operator=(const Subscription&) = delete;

    // Spawns the worker thread. Idempotent; second call is a no-op.
    void start();

    // Signals the worker to exit and joins it. Idempotent and safe to call
    // from any thread (including the worker itself, in which case it just
    // sets the flag without joining).
    void stop();

    const std::string& id() const noexcept { return id_; }

protected:
    // Subclass entry point — runs on the background thread.
    virtual void run() = 0;

    // Emits an EVENT frame with a UTF-8 JSON payload.
    void emit(std::string_view json);

    // Emits an EVENT frame with arbitrary bytes (e.g. a captured image).
    void emit_bytes(std::span<const std::byte> bytes);

    bool should_stop() const noexcept { return stop_requested_.load(); }

private:
    wire::Writer&       writer_;
    std::string         id_;
    std::atomic<bool>   stop_requested_{false};
    std::atomic<bool>   started_{false};
    std::thread         thread_;
};

class SubscriptionRegistry {
public:
    SubscriptionRegistry();
    ~SubscriptionRegistry();

    // Allocates a fresh subscription id of the form `sub:<n>`.
    std::string allocate_id();

    // Registers a (started) subscription. Takes ownership.
    void register_subscription(std::unique_ptr<Subscription> sub);

    // Cancels and removes a subscription. Idempotent on unknown ids.
    // Returns true if found and cancelled.
    bool cancel(std::string_view id);

    // Cancels all active subscriptions and joins their threads. Called from
    // Connection's close handler to satisfy PROTOCOL.md §2.5 drain semantics.
    void cancel_all();

private:
    std::mutex                                                       mu_;
    unsigned                                                         next_id_ = 1;
    std::unordered_map<std::string, std::unique_ptr<Subscription>>   subs_;
};

}  // namespace remote_hands

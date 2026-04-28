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

#include "subscription.hpp"

#include "log.hpp"

#include <cstdio>
#include <utility>

namespace remote_hands {

// ---------------------------------------------------------------------------
// Subscription

Subscription::Subscription(wire::Writer& writer, std::string id)
    : writer_{writer}, id_{std::move(id)} {}

Subscription::~Subscription() {
    stop();
}

void Subscription::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) return;

    thread_ = std::thread([this] {
        try {
            run();
        } catch (const std::exception& ex) {
            log::warning(L"Subscription %hs threw: %hs",
                         id_.c_str(), ex.what());
        }
    });
}

void Subscription::stop() {
    stop_requested_.store(true);
    if (thread_.joinable()) {
        if (std::this_thread::get_id() == thread_.get_id()) {
            // Stop called from the worker itself — detach so we don't deadlock.
            thread_.detach();
        } else {
            thread_.join();
        }
    }
}

void Subscription::emit(std::string_view json) {
    try {
        writer_.write_event(id_, json);
    } catch (const std::exception& ex) {
        log::warning(L"emit (json) failed for %hs: %hs",
                     id_.c_str(), ex.what());
        stop_requested_.store(true);
    }
}

void Subscription::emit_bytes(std::span<const std::byte> bytes) {
    try {
        writer_.write_event(id_, bytes);
    } catch (const std::exception& ex) {
        log::warning(L"emit (bytes) failed for %hs: %hs",
                     id_.c_str(), ex.what());
        stop_requested_.store(true);
    }
}

// ---------------------------------------------------------------------------
// SubscriptionRegistry

SubscriptionRegistry::SubscriptionRegistry() = default;

SubscriptionRegistry::~SubscriptionRegistry() {
    cancel_all();
}

std::string SubscriptionRegistry::allocate_id() {
    std::lock_guard lock{mu_};
    char buf[32];
    std::snprintf(buf, sizeof(buf), "sub:%u", next_id_++);
    return buf;
}

void SubscriptionRegistry::register_subscription(std::unique_ptr<Subscription> sub) {
    if (!sub) return;
    std::lock_guard lock{mu_};
    subs_.emplace(sub->id(), std::move(sub));
}

bool SubscriptionRegistry::cancel(std::string_view id) {
    std::unique_ptr<Subscription> victim;
    {
        std::lock_guard lock{mu_};
        auto it = subs_.find(std::string{id});
        if (it == subs_.end()) return false;
        victim = std::move(it->second);
        subs_.erase(it);
    }
    // victim's destructor calls stop() and joins. Done outside the lock so a
    // worker that calls back into the writer (or otherwise blocks briefly)
    // doesn't hold the registry mutex.
    return true;
}

void SubscriptionRegistry::cancel_all() {
    std::unordered_map<std::string, std::unique_ptr<Subscription>> drained;
    {
        std::lock_guard lock{mu_};
        drained.swap(subs_);
    }
    drained.clear();  // destruct outside the lock
}

}  // namespace remote_hands

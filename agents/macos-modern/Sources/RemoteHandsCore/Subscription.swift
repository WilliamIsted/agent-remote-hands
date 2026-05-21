//
// Copyright 2026 William Isted and contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

import Foundation

/// One active `watch.*` subscription. Wire identity is `sub:N`. Cancellation
/// releases the underlying resource (kqueue fd, FSEventStream, AXObserver,
/// DispatchSourceTimer, …) and removes the entry from its registry.
public protocol RHSubscription: AnyObject, Sendable {
    var id: String { get }
    func cancel()
}

/// Per-connection registry of `watch.*` subscriptions. Thread-safe so
/// watcher callbacks can register / lookup / cancel from arbitrary queues.
public final class SubscriptionRegistry: @unchecked Sendable {
    private var subs: [String: RHSubscription] = [:]
    private var nextID = 1
    private let lock = NSLock()

    public init() {}

    /// Allocate the next sub-id without registering yet (caller owns the
    /// subscription object's lifetime and may decide not to add it).
    public func nextSubID() -> String {
        lock.lock(); defer { lock.unlock() }
        let id = "sub:\(nextID)"
        nextID += 1
        return id
    }

    public func add(_ sub: RHSubscription) {
        lock.lock(); defer { lock.unlock() }
        subs[sub.id] = sub
    }

    public func cancel(id: String) -> Bool {
        lock.lock()
        let sub = subs.removeValue(forKey: id)
        lock.unlock()
        guard let sub = sub else { return false }
        sub.cancel()
        return true
    }

    /// Cancel every active subscription. Called when the owning connection
    /// closes.
    public func cancelAll() {
        lock.lock()
        let all = Array(subs.values)
        subs.removeAll()
        lock.unlock()
        for sub in all { sub.cancel() }
    }
}

/// Context passed through to every verb handler. Holds per-connection state
/// the dispatcher needs to expose: current tier, element table, the
/// subscription registry, and the EVENT-frame send callback.
public struct DispatchContext {
    public let currentTier: Tier
    public let elementTable: ElementTable
    public let subscriptions: SubscriptionRegistry
    /// Send an EVENT frame for a subscription. Thread-safe — the
    /// connection-level send mutex serialises this against the regular
    /// response writes.
    public let sendEvent: @Sendable (String, Data) -> Void

    public init(currentTier: Tier, elementTable: ElementTable,
                subscriptions: SubscriptionRegistry,
                sendEvent: @escaping @Sendable (String, Data) -> Void) {
        self.currentTier = currentTier
        self.elementTable = elementTable
        self.subscriptions = subscriptions
        self.sendEvent = sendEvent
    }
}

/// Format an `EVENT <sub-id> <length>\n<payload>` frame per PROTOCOL.md §6.1.
public func formatEvent(subID: String, payload: Data) -> Data {
    var out = Data()
    let header = "EVENT \(subID) \(payload.count)\n"
    out.append(header.data(using: .utf8)!)
    out.append(payload)
    return out
}

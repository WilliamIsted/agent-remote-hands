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

/// Sendable-tolerant container used for sync↔async bridging. The verb
/// dispatcher runs synchronously per connection but several macOS frameworks
/// (ScreenCaptureKit, Vision, URLSession) are async-only; bridging via
/// `Task.detached` + `DispatchSemaphore` requires a Sendable handoff slot,
/// which this provides.
public final class ResultBox<T>: @unchecked Sendable {
    private var value: Result<T, Error>?
    private let lock = NSLock()

    public init() {}

    public func set(_ r: Result<T, Error>) {
        lock.lock(); defer { lock.unlock() }
        value = r
    }

    public func get() -> Result<T, Error>? {
        lock.lock(); defer { lock.unlock() }
        return value
    }
}

/// Run an async operation synchronously, returning its result or rethrowing
/// its error. Pattern is `Task.detached` + `DispatchSemaphore` + `ResultBox`.
///
/// This is the standard bridge for the verb-dispatcher world (sync,
/// per-connection thread) calling into modern async-only macOS APIs. Do not
/// use it from inside an async context — it'll deadlock if the current
/// executor is a cooperative thread pool with no other workers available.
public func runBlocking<T>(_ op: @Sendable @escaping () async throws -> T) throws -> T {
    let box = ResultBox<T>()
    let sem = DispatchSemaphore(value: 0)
    Task.detached {
        do {
            let v = try await op()
            box.set(.success(v))
        } catch {
            box.set(.failure(error))
        }
        sem.signal()
    }
    sem.wait()
    switch box.get() {
    case .success(let v): return v
    case .failure(let e): throw e
    case .none:
        // Unreachable — the Task always calls box.set before signal.
        fatalError("runBlocking: semaphore signalled with no result")
    }
}

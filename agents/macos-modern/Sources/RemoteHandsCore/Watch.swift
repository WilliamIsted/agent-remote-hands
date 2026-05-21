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
import AppKit
import CoreServices
import ApplicationServices
import CoreGraphics
#if canImport(Darwin)
import Darwin
#endif

public enum WatchError: Error, Equatable {
    case notFound
    case axDisabled
    case unsupported
    case invalidArgs(String)
}

// MARK: - watch.process

final class ProcessExitSubscription: RHSubscription, @unchecked Sendable {
    let id: String
    private let queue = DispatchQueue(label: "rha-mac.watch.process")
    private var source: DispatchSourceProcess?
    private var cancelled = false
    private let lock = NSLock()

    init?(id: String, pid: Int32, send: @escaping @Sendable (String, Data) -> Void) {
        self.id = id
        let src = DispatchSource.makeProcessSource(identifier: pid_t(pid), eventMask: .exit, queue: queue)
        src.setEventHandler { [weak self] in
            guard let self = self else { return }
            self.lock.lock(); let stop = self.cancelled; self.lock.unlock()
            if stop { return }
            let body: [String: Any] = ["event": "exit", "pid": Int(pid)]
            let data = (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
            send(id, data)
            self.cancel()
        }
        src.resume()
        self.source = src
    }

    func cancel() {
        lock.lock()
        if cancelled { lock.unlock(); return }
        cancelled = true
        let s = source
        source = nil
        lock.unlock()
        s?.cancel()
    }
}

// MARK: - watch.file

final class FileWatchSubscription: RHSubscription, @unchecked Sendable {
    let id: String
    private var stream: FSEventStreamRef?
    private let send: @Sendable (String, Data) -> Void
    private let glob: String
    private let queue = DispatchQueue(label: "rha-mac.watch.file")
    private var cancelled = false
    private let lock = NSLock()

    init?(id: String, glob: String, send: @escaping @Sendable (String, Data) -> Void) {
        self.id = id
        // Canonicalise the pattern so /tmp/* matches /private/tmp/* events
        // (FSEvents reports realpath'd absolute paths).
        let dir = (glob as NSString).deletingLastPathComponent
        let canonicalDir = dir.isEmpty ? FileManager.default.currentDirectoryPath
            : URL(fileURLWithPath: dir).resolvingSymlinksInPath().path
        let leaf = (glob as NSString).lastPathComponent
        self.glob = (canonicalDir as NSString).appendingPathComponent(leaf)
        self.send = send
        let watchPath = canonicalDir

        var context = FSEventStreamContext(
            version: 0,
            info: Unmanaged.passUnretained(self).toOpaque(),
            retain: nil,
            release: nil,
            copyDescription: nil
        )
        let cb: FSEventStreamCallback = { _, info, count, paths, flags, _ in
            guard let info = info else { return }
            let me = Unmanaged<FileWatchSubscription>.fromOpaque(info).takeUnretainedValue()
            let pathPtr = paths.assumingMemoryBound(to: UnsafePointer<CChar>.self)
            let flagPtr = flags
            for i in 0..<count {
                let path = String(cString: pathPtr[i])
                let f = flagPtr[i]
                let kind: String
                if (f & UInt32(kFSEventStreamEventFlagItemCreated)) != 0 { kind = "created" }
                else if (f & UInt32(kFSEventStreamEventFlagItemRemoved)) != 0 { kind = "deleted" }
                else if (f & UInt32(kFSEventStreamEventFlagItemModified)) != 0 { kind = "modified" }
                else if (f & UInt32(kFSEventStreamEventFlagItemRenamed)) != 0 { kind = "renamed" }
                else { kind = "changed" }
                // Filter by glob match against full path.
                if !me.matches(path: path) { continue }
                let body: [String: Any] = ["event": kind, "path": path]
                let data = (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
                me.send(me.id, data)
            }
        }
        let s = FSEventStreamCreate(
            kCFAllocatorDefault,
            cb,
            &context,
            [watchPath] as CFArray,
            FSEventStreamEventId(kFSEventStreamEventIdSinceNow),
            0.1,
            // No UseCFTypes — the callback reads `paths` as a C array of
            // C strings via assumingMemoryBound(to: UnsafePointer<CChar>).
            UInt32(kFSEventStreamCreateFlagFileEvents)
        )
        guard let s = s else { return nil }
        self.stream = s
        FSEventStreamSetDispatchQueue(s, queue)
        FSEventStreamStart(s)
    }

    private func matches(path: String) -> Bool {
        // Reuse Window's glob matcher.
        return Window.globMatch(pattern: glob, in: path)
    }

    func cancel() {
        lock.lock()
        if cancelled { lock.unlock(); return }
        cancelled = true
        let s = stream; stream = nil
        lock.unlock()
        if let s = s {
            FSEventStreamStop(s)
            FSEventStreamInvalidate(s)
            FSEventStreamRelease(s)
        }
    }
}

// MARK: - watch.window

final class WindowWatchSubscription: RHSubscription, @unchecked Sendable {
    let id: String
    private var observers: [NSObjectProtocol] = []
    private let titlePrefix: String?
    private let send: @Sendable (String, Data) -> Void

    init(id: String, titlePrefix: String?, send: @escaping @Sendable (String, Data) -> Void) {
        self.id = id
        self.titlePrefix = titlePrefix
        self.send = send
        let center = NSWorkspace.shared.notificationCenter
        let launch = center.addObserver(forName: NSWorkspace.didLaunchApplicationNotification, object: nil, queue: nil) { [weak self] note in
            guard let self = self,
                  let app = note.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication
            else { return }
            self.maybeSend(event: "appeared", app: app)
        }
        let term = center.addObserver(forName: NSWorkspace.didTerminateApplicationNotification, object: nil, queue: nil) { [weak self] note in
            guard let self = self,
                  let app = note.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication
            else { return }
            self.maybeSend(event: "disappeared", app: app)
        }
        observers = [launch, term]
    }

    private func maybeSend(event: String, app: NSRunningApplication) {
        let name = app.localizedName ?? ""
        if let prefix = titlePrefix, !name.hasPrefix(prefix) { return }
        let body: [String: Any] = [
            "event": event,
            "app": name,
            "pid": app.processIdentifier,
        ]
        let data = (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
        send(id, data)
    }

    func cancel() {
        let center = NSWorkspace.shared.notificationCenter
        for o in observers { center.removeObserver(o) }
        observers.removeAll()
    }
}

// MARK: - watch.region (polling)

final class RegionWatchSubscription: RHSubscription, @unchecked Sendable {
    let id: String
    private var timer: DispatchSourceTimer?
    private let queue = DispatchQueue(label: "rha-mac.watch.region")
    private var lastHash: Int = 0
    private let untilChange: Bool
    private let send: @Sendable (String, Data) -> Void
    private weak var registry: SubscriptionRegistry?

    init(id: String, intervalMs: Int, untilChange: Bool, registry: SubscriptionRegistry?, send: @escaping @Sendable (String, Data) -> Void) {
        self.id = id
        self.untilChange = untilChange
        self.registry = registry
        self.send = send
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + .milliseconds(intervalMs), repeating: .milliseconds(intervalMs))
        t.setEventHandler { [weak self] in
            self?.tick()
        }
        t.resume()
        self.timer = t
    }

    private func tick() {
        do {
            // For MVP region watching we capture the full screen and hash
            // the bytes. A future slice will switch to SCStream for
            // per-frame callbacks and skip the encode round-trip.
            let bytes = try ScreenCapture.captureFullScreen(format: .png, quality: 75)
            let h = bytes.hashValue
            if h == lastHash { return }
            lastHash = h
            send(id, bytes)
            if untilChange {
                if let registry = registry {
                    _ = registry.cancel(id: id)
                } else {
                    cancel()
                }
            }
        } catch {
            // Silently skip frames on capture errors (TCC denial etc.) so the
            // subscription doesn't tight-loop. Cancellation is up to the
            // caller via watch.cancel.
            return
        }
    }

    func cancel() {
        timer?.cancel()
        timer = nil
    }
}

// MARK: - watch.element

final class ElementWatchSubscription: RHSubscription, @unchecked Sendable {
    let id: String
    private var observer: AXObserver?
    private let element: AXUIElement
    private let pid: pid_t
    private let send: @Sendable (String, Data) -> Void
    private let lock = NSLock()
    private var cancelled = false

    init?(id: String, pid: pid_t, element: AXUIElement, send: @escaping @Sendable (String, Data) -> Void) {
        self.id = id
        self.element = element
        self.pid = pid
        self.send = send

        var observer: AXObserver?
        let opaque = Unmanaged.passUnretained(self).toOpaque()
        let cb: AXObserverCallback = { _, _, notification, refcon in
            guard let refcon = refcon else { return }
            let me = Unmanaged<ElementWatchSubscription>.fromOpaque(refcon).takeUnretainedValue()
            let body: [String: Any] = [
                "event": notification as String,
                "elt": me.id,
            ]
            let data = (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
            me.send(me.id, data)
        }
        let err = AXObserverCreate(pid, cb, &observer)
        guard err == .success, let obs = observer else { return nil }
        self.observer = obs
        let attachErr = AXObserverAddNotification(obs, element, kAXValueChangedNotification as CFString, opaque)
        if attachErr != .success { return nil }
        CFRunLoopAddSource(CFRunLoopGetMain(), AXObserverGetRunLoopSource(obs), .defaultMode)
    }

    func cancel() {
        lock.lock()
        if cancelled { lock.unlock(); return }
        cancelled = true
        let obs = observer
        observer = nil
        lock.unlock()
        if let obs = obs {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), AXObserverGetRunLoopSource(obs), .defaultMode)
        }
    }
}

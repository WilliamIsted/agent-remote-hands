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
import IOKit
import IOKit.pwr_mgt

public enum PowerError: Error, Equatable {
    case permissionDenied
    case notFound
    case unsupported
    /// Power verbs that change machine state (shutdown / reboot / logoff /
    /// sleep / hibernate) are GATED OFF by default and require the agent
    /// to be started with `--allow-power-state-changes`. Without that
    /// flag, the verbs refuse — protects the host from accidental or
    /// conformance-suite-driven state changes.
    case powerStateChangesDisabled
    case io(String)
}

/// Process-wide flag that gates the genuinely destructive power verbs.
/// Set once at startup by main.swift if the user passes
/// `--allow-power-state-changes`. Default: disabled.
public enum PowerPolicy {
    nonisolated(unsafe) public static var allowStateChanges: Bool = false
}

public struct PowerBlocker: Sendable {
    public let pid: Int32
    public let type: String
    public let level: Int
    public let reason: String

    public var jsonObject: [String: Any] {
        ["pid": Int(pid), "type": type, "level": level, "reason": reason]
    }
}

/// `system.power.*` implementations.
public enum Power {

    // MARK: blockers (R)

    public static func blockers() -> [PowerBlocker] {
        var assertions: Unmanaged<CFDictionary>?
        let result = IOPMCopyAssertionsByProcess(&assertions)
        if result != kIOReturnSuccess { return [] }
        guard let raw = assertions?.takeRetainedValue() as? [AnyHashable: Any] else { return [] }
        var out: [PowerBlocker] = []
        // Outer dict is pid → [assertion dicts].
        for (pidKey, value) in raw {
            let pid: Int32
            if let n = pidKey as? Int { pid = Int32(n) }
            else if let s = pidKey as? String, let n = Int32(s) { pid = n }
            else { continue }
            guard let list = value as? [[String: Any]] else { continue }
            for entry in list {
                let type = (entry[kIOPMAssertionTypeKey as String] as? String) ?? ""
                // Only assertions that prevent sleep/shutdown are interesting
                // to callers. Keep the canonical sleep-blocker types.
                let sleepBlockers: Set<String> = [
                    "PreventUserIdleSystemSleep",
                    "PreventSystemSleep",
                    "NoIdleSleepAssertion",
                    "ApplePushServiceTask",
                ]
                if !sleepBlockers.contains(type) { continue }
                let level = (entry[kIOPMAssertionLevelKey as String] as? Int) ?? 0
                let reason = (entry["AssertName" as String] as? String)
                          ?? (entry["AssertNameProcessName" as String] as? String)
                          ?? ""
                out.append(PowerBlocker(pid: pid, type: type, level: level, reason: reason))
            }
        }
        return out
    }

    // MARK: lock (R)

    /// Lock the workstation. macOS removed `CGSession` from the user-
    /// accessible binaries in Tahoe. The most reliable no-TCC modern path
    /// is `pmset displaysleepnow`, which sleeps the display; if "require
    /// password immediately after sleep or screen saver begins" is set in
    /// System Settings → Lock Screen, this locks the workstation.
    public static func lock() throws {
        try run(executable: "/usr/bin/pmset", args: ["displaysleepnow"])
    }

    // MARK: shutdown / reboot / logoff (X) — gated

    /// All of shutdown / reboot / logoff / sleep / hibernate are gated on
    /// `PowerPolicy.allowStateChanges`. By default the agent REFUSES any
    /// machine-state-changing power verb regardless of tier — protects the
    /// host from conformance suites + accidental remote sessions. Operator
    /// passes `--allow-power-state-changes` to the agent to opt in.

    public static func shutdown(force: Bool = false) throws {
        try requirePolicy()
        try runOsascript("tell application \"System Events\" to shut down")
    }

    public static func reboot(force: Bool = false) throws {
        try requirePolicy()
        try runOsascript("tell application \"System Events\" to restart")
    }

    public static func logoff(force: Bool = false) throws {
        try requirePolicy()
        try runOsascript("tell application \"System Events\" to log out")
    }

    public static func hibernate() throws {
        try requirePolicy()
        try sleepImpl()
    }

    public static func sleep() throws {
        try requirePolicy()
        try sleepImpl()
    }

    private static func sleepImpl() throws {
        try run(executable: "/usr/bin/pmset", args: ["sleepnow"])
    }

    private static func requirePolicy() throws {
        if !PowerPolicy.allowStateChanges {
            throw PowerError.powerStateChangesDisabled
        }
    }

    // MARK: helpers

    private static func runOsascript(_ script: String) throws {
        try run(executable: "/usr/bin/osascript", args: ["-e", script])
    }

    private static func run(executable: String, args: [String]) throws {
        let task = Foundation.Process()
        task.executableURL = URL(fileURLWithPath: executable)
        task.arguments = args
        do {
            try task.run()
            task.waitUntilExit()
        } catch let e as NSError {
            if e.code == NSFileReadNoSuchFileError { throw PowerError.notFound }
            throw PowerError.io(e.localizedDescription)
        } catch {
            throw PowerError.io("\(error)")
        }
        if task.terminationStatus != 0 {
            throw PowerError.io("\(executable) exited with status \(task.terminationStatus)")
        }
    }
}

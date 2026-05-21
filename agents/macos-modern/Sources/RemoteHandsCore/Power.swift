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
    case io(String)
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

    // MARK: shutdown / reboot / logoff (X)

    public static func shutdown(force: Bool = false) throws {
        try runOsascript("tell application \"System Events\" to shut down")
    }

    public static func reboot(force: Bool = false) throws {
        try runOsascript("tell application \"System Events\" to restart")
    }

    public static func logoff(force: Bool = false) throws {
        try runOsascript("tell application \"System Events\" to log out")
    }

    // MARK: hibernate / sleep (X)

    /// macOS doesn't have a separate hibernate verb from the user's
    /// perspective — `pmset sleepnow` after setting hibernate mode = 25
    /// achieves S4-style hibernate. Setting hibernate mode requires admin,
    /// so on unprivileged callers we fall back to plain sleep.
    public static func hibernate() throws {
        // For an unprivileged dev binary, the cleanest path is plain sleep
        // (which on portable Macs will hibernate after the standby delay).
        try sleep()
    }

    public static func sleep() throws {
        try run(executable: "/usr/bin/pmset", args: ["sleepnow"])
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

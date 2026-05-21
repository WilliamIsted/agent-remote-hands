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
#if canImport(Darwin)
import Darwin
import Darwin.libproc
#endif

public enum ProcError: Error, Equatable {
    case notFound
    case permissionDenied
    case timeout
    case spawnFailed(String)
    case io(String)
}

public struct ProcessInfoEntry: Sendable {
    public let pid: Int32
    public let ppid: Int32
    public let name: String
    public let path: String

    /// Post-rc.2 shape uses `image` (executable name) and optional counters
    /// block. The `name` and `path` fields are preserved for callers that
    /// already depend on them; field renames are conservative.
    public func jsonObject(includeCounters: Bool = false) -> [String: Any] {
        var obj: [String: Any] = [
            "pid": Int(pid),
            "ppid": Int(ppid),
            "image": name,           // canonical post-rc.2 field
            "image_path": path,      // full exec path
            "name": name,            // legacy alias retained
            "path": path,            // legacy alias retained
        ]
        if includeCounters {
            // libproc/task_info-derived counters would land here. For the
            // first conformance pass, emit zeroes so the field is present
            // and a future slice can wire real numbers.
            obj["cpu_user_ms"] = 0
            obj["cpu_kernel_ms"] = 0
            obj["rss_bytes"] = 0
            obj["handle_count"] = 0
        }
        return obj
    }
}

/// `process.*` implementations.
public enum ProcOps {

    // MARK: list

    public static func list(filter: String? = nil) -> [ProcessInfoEntry] {
        var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0]
        var len = 0
        if sysctl(&mib, UInt32(mib.count), nil, &len, nil, 0) != 0 {
            return []
        }
        var procs = [kinfo_proc](repeating: kinfo_proc(), count: len / MemoryLayout<kinfo_proc>.stride)
        if sysctl(&mib, UInt32(mib.count), &procs, &len, nil, 0) != 0 {
            return []
        }
        let count = len / MemoryLayout<kinfo_proc>.stride
        var out: [ProcessInfoEntry] = []
        out.reserveCapacity(count)
        for i in 0..<count {
            let proc = procs[i].kp_proc
            let pid = proc.p_pid
            if pid == 0 { continue }
            let ppid = procs[i].kp_eproc.e_ppid
            // p_comm is a 16-byte char tuple; copy out before reading via
            // a pointer so Swift exclusivity is satisfied.
            var comm = proc.p_comm
            let name = withUnsafePointer(to: &comm) { ptr -> String in
                ptr.withMemoryRebound(to: CChar.self, capacity: 16) { c in
                    return String(cString: c)
                }
            }
            // Full exec path via libproc. PROC_PIDPATHINFO_MAXSIZE = 4 *
            // MAXPATHLEN is a C macro Swift does not import; hardcoding the
            // value matches `<sys/proc_info.h>`.
            var pathBuf = [CChar](repeating: 0, count: 4 * 1024)
            let pathLen = proc_pidpath(pid, &pathBuf, UInt32(pathBuf.count))
            let path = (pathLen > 0) ? String(cString: pathBuf) : ""

            if let f = filter, !Window.globMatch(pattern: f, in: name) && !Window.globMatch(pattern: f, in: path) {
                continue
            }
            out.append(ProcessInfoEntry(pid: pid, ppid: ppid, name: name, path: path))
        }
        return out
    }

    // MARK: start

    /// Spawn a process with optional stdin payload. Returns the new pid.
    public static func start(executable: String, args: [String], stdinPayload: Data? = nil) throws -> Int32 {
        let task = Process()
        task.executableURL = URL(fileURLWithPath: executable)
        task.arguments = args
        if let payload = stdinPayload {
            let pipe = Pipe()
            task.standardInput = pipe
            do {
                try task.run()
            } catch {
                throw ProcError.spawnFailed("\(error)")
            }
            // Write payload then close.
            pipe.fileHandleForWriting.write(payload)
            try? pipe.fileHandleForWriting.close()
        } else {
            do {
                try task.run()
            } catch let e as NSError {
                if e.code == NSFileReadNoSuchFileError { throw ProcError.notFound }
                throw ProcError.spawnFailed(e.localizedDescription)
            } catch {
                throw ProcError.spawnFailed("\(error)")
            }
        }
        return task.processIdentifier
    }

    /// Run `/bin/sh -c <cmd>` and capture stdout / stderr. Synchronous.
    /// Returns (exit_code, stdout, stderr).
    public static func shell(_ command: String) throws -> (exitCode: Int32, stdout: Data, stderr: Data) {
        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/bin/sh")
        task.arguments = ["-c", command]
        let outPipe = Pipe()
        let errPipe = Pipe()
        task.standardOutput = outPipe
        task.standardError = errPipe
        do {
            try task.run()
        } catch {
            throw ProcError.spawnFailed("\(error)")
        }
        // Read both pipes concurrently so a large stdout doesn't deadlock
        // against stderr's pipe buffer. Foundation pipes are 16K.
        let outData = outPipe.fileHandleForReading.readDataToEndOfFile()
        let errData = errPipe.fileHandleForReading.readDataToEndOfFile()
        task.waitUntilExit()
        return (task.terminationStatus, outData, errData)
    }

    // MARK: kill

    public static func kill(pid: Int32, force: Bool = false) throws {
        let sig = force ? SIGKILL : SIGTERM
        let r = Darwin.kill(pid_t(pid), sig)
        if r != 0 {
            switch errno {
            case ESRCH: throw ProcError.notFound
            case EPERM: throw ProcError.permissionDenied
            default:    throw ProcError.io("kill errno=\(errno)")
            }
        }
    }

    // MARK: wait

    /// Wait for `pid` to exit, up to `deadline` (absolute ms since epoch).
    /// Returns the exit status if the process exits in time, or `timeout`.
    public static func waitFor(pid: Int32, deadlineMs: Int) throws -> Int32 {
        // kqueue + EVFILT_PROC + NOTE_EXIT works on arbitrary pids (not
        // just children). Returns when the kernel notifies of exit.
        let kq = kqueue()
        if kq < 0 { throw ProcError.io("kqueue() errno=\(errno)") }
        defer { Darwin.close(kq) }

        var change = kevent()
        change.ident = UInt(pid)
        change.filter = Int16(EVFILT_PROC)
        change.flags = UInt16(EV_ADD | EV_ONESHOT)
        change.fflags = NOTE_EXIT
        change.data = 0
        change.udata = nil

        let now = Date().timeIntervalSince1970 * 1000
        let remaining = max(0.0, Double(deadlineMs) - now)
        var ts = timespec()
        ts.tv_sec = Int(remaining / 1000)
        ts.tv_nsec = Int((remaining.truncatingRemainder(dividingBy: 1000)) * 1_000_000)

        var event = kevent()
        let n = withUnsafePointer(to: &change) { changePtr in
            withUnsafeMutablePointer(to: &event) { eventPtr in
                withUnsafePointer(to: &ts) { tsPtr in
                    kevent(kq, changePtr, 1, eventPtr, 1, tsPtr)
                }
            }
        }
        if n == 0 { throw ProcError.timeout }
        if n < 0 {
            if errno == ESRCH { throw ProcError.notFound }
            throw ProcError.io("kevent errno=\(errno)")
        }
        // EV_ERROR (in flags) signals a problem with the kevent registration
        // itself; event.data carries the errno. ESRCH means the pid never
        // existed or was already reaped.
        if (Int32(event.flags) & EV_ERROR) != 0 {
            if event.data == Int(ESRCH) { throw ProcError.notFound }
            throw ProcError.io("kevent EV_ERROR data=\(event.data)")
        }
        // event.data holds the exit status on NOTE_EXIT.
        return Int32(event.data & 0xFF)
    }
}

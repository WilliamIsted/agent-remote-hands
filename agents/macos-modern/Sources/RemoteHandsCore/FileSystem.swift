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
#endif

// File-scope wrappers around `stat(2)` / `lstat(2)` / `rmdir(2)` /
// `unlink(2)` etc. Inside `enum FileSystem` below, plain `stat` resolves to
// a member lookup (and fails), so callers go through these helpers
// instead. The wrappers live at file scope where Swift resolves the bare
// names to the Darwin globals correctly.
fileprivate func sysStat(_ path: String, _ buf: UnsafeMutablePointer<Darwin.stat>) -> Int32 {
    return path.withCString { stat($0, buf) }
}

fileprivate func sysLstat(_ path: String, _ buf: UnsafeMutablePointer<Darwin.stat>) -> Int32 {
    return path.withCString { lstat($0, buf) }
}

public enum FSError: Error, Equatable {
    case notFound
    case alreadyExists
    case notADirectory
    case notEmpty
    case permissionDenied
    /// Path is protected by the agent's FileGuard policy (token file,
    /// keychain, SSH keys, etc.). Distinct from `permissionDenied` so the
    /// wire response can report `category: protected_path` to clients.
    case protectedPath
    case crossDevice
    case timeout
    case io(String)
}

public enum FSEntryType: String, Sendable {
    case file
    case directory
    case symlink
    case other
}

public struct StatInfo: Sendable {
    public let path: String
    public let type: FSEntryType
    public let size: Int64
    public let mtime: Double
    public let mode: UInt16

    public var jsonObject: [String: Any] {
        [
            "path": path,
            "type": type.rawValue,
            "size": size,
            "mtime": mtime,
            "mode": String(format: "%o", mode),
        ]
    }
}

/// `file.*` + `directory.*` implementations on POSIX + Foundation.
/// Paths are UTF-8 throughout.
public enum FileSystem {

    private static let fm = FileManager.default

    // MARK: stat / exists

    public static func stat(_ path: String) throws -> StatInfo {
        try FileGuard.ensureAllowed(path)
        var st = Darwin.stat()
        if sysStat(path, &st) != 0 {
            throw errToFSError(errno: errno)
        }
        return statInfo(path: path, st: st)
    }

    public static func exists(_ path: String) -> (exists: Bool, type: FSEntryType?) {
        // Protected paths report as not-existing rather than denied so a
        // probing client can't even confirm the file is there. Verb-level
        // exists handler turns this into the same wire shape as a real
        // miss — callers needing distinction should use file.stat which
        // returns ERR permission_denied with category=protected_path.
        if FileGuard.isProtected(path) { return (false, nil) }
        // Use stat (follows symlinks) so /tmp → /private/tmp resolves to
        // type=directory, not type=symlink. Callers needing symlink
        // detection should use file.stat (which we also use stat() for in
        // this MVP — lstat exposure can come later).
        var st = Darwin.stat()
        if sysStat(path, &st) != 0 { return (false, nil) }
        return (true, entryType(mode: mode_t(st.st_mode)))
    }

    // MARK: file ops

    public static func createFile(_ path: String) throws {
        try FileGuard.ensureAllowed(path)
        let fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0o644)
        if fd < 0 {
            throw errToFSError(errno: errno)
        }
        Darwin.close(fd)
    }

    public static func readFile(_ path: String) throws -> Data {
        try FileGuard.ensureAllowed(path)
        let url = URL(fileURLWithPath: path)
        do {
            return try Data(contentsOf: url, options: [.mappedIfSafe])
        } catch let e as NSError {
            switch e.code {
            case NSFileReadNoSuchFileError: throw FSError.notFound
            case NSFileReadNoPermissionError: throw FSError.permissionDenied
            default: throw FSError.io(e.localizedDescription)
            }
        }
    }

    public static func writeFile(_ path: String, payload: Data) throws {
        try FileGuard.ensureAllowed(path)
        let url = URL(fileURLWithPath: path)
        do {
            try payload.write(to: url, options: [.atomic])
        } catch let e as NSError {
            switch e.code {
            case NSFileWriteNoPermissionError: throw FSError.permissionDenied
            default: throw FSError.io(e.localizedDescription)
            }
        }
    }

    public static func writeAt(_ path: String, offset: Int64, payload: Data, truncate: Bool = false) throws {
        try FileGuard.ensureAllowed(path)
        var flags: Int32 = O_WRONLY
        if truncate && offset == 0 { flags |= O_TRUNC }
        let fd = open(path, flags)
        if fd < 0 { throw errToFSError(errno: errno) }
        defer { Darwin.close(fd) }
        if lseek(fd, off_t(offset), SEEK_SET) < 0 {
            throw errToFSError(errno: errno)
        }
        try payload.withUnsafeBytes { raw -> Void in
            guard let base = raw.baseAddress else { return }
            var remaining = payload.count
            var p = base
            while remaining > 0 {
                let n = Darwin.write(fd, p, remaining)
                if n <= 0 { throw errToFSError(errno: errno) }
                p = p.advanced(by: n); remaining -= n
            }
        }
    }

    /// Outcome of a delete operation — distinguishes Trash from permanent
    /// so the wire response can confirm what actually happened.
    public struct DeleteOutcome: Sendable {
        public let mode: DeleteMode
        /// `file://` URL of the file inside Trash. Always nil for
        /// `.permanent`; sometimes nil for `.trashed` if FileManager
        /// declines to surface it (rare).
        public let trashURL: URL?
    }

    public enum DeleteMode: String, Sendable {
        case trashed
        case permanent
    }

    /// Delete a file or empty directory. Defaults to moving the target to
    /// Trash (recoverable); `permanent: true` opts into `unlink(2)` /
    /// `rmdir(2)` for an unrecoverable delete.
    @discardableResult
    public static func deleteFile(_ path: String, permanent: Bool = false) throws -> DeleteOutcome {
        try FileGuard.ensureAllowed(path)
        if !permanent {
            // Trash via FileManager. trashItem handles both files and
            // directories (single-entry only — non-empty dirs go via
            // directory.remove --recursive). It returns a resultingItemURL
            // that names the moved file inside Trash.
            var resultingURL: NSURL?
            do {
                try fm.trashItem(at: URL(fileURLWithPath: path), resultingItemURL: &resultingURL)
                return DeleteOutcome(mode: .trashed, trashURL: resultingURL as URL?)
            } catch let e as NSError {
                switch e.code {
                case NSFileNoSuchFileError, NSFileReadNoSuchFileError: throw FSError.notFound
                case NSFileWriteNoPermissionError: throw FSError.permissionDenied
                default: throw FSError.io(e.localizedDescription)
                }
            }
        }
        // permanent=true → POSIX hard delete.
        if unlink(path) != 0 {
            // unlink fails on directories with EPERM/EISDIR; if it's a dir,
            // try rmdir for empty-directory parity with PROTOCOL.md's
            // "file or empty directory" wording.
            if errno == EISDIR || errno == EPERM {
                if rmdir(path) == 0 {
                    return DeleteOutcome(mode: .permanent, trashURL: nil)
                }
                if errno == ENOTEMPTY { throw FSError.notEmpty }
            }
            throw errToFSError(errno: errno)
        }
        return DeleteOutcome(mode: .permanent, trashURL: nil)
    }

    public static func rename(src: String, dst: String, overwrite: Bool = false, allowCrossFS: Bool = false) throws {
        // Both ends of a rename must be allowed — otherwise an attacker
        // could move the token file out of its protected dir, or write a
        // chosen file into one.
        try FileGuard.ensureAllowed(src)
        try FileGuard.ensureAllowed(dst)
        if !overwrite {
            var st = Darwin.stat()
            if lstat(dst, &st) == 0 { throw FSError.alreadyExists }
        }
        if Darwin.rename(src, dst) == 0 { return }
        if errno == EXDEV {
            if !allowCrossFS { throw FSError.crossDevice }
            do {
                try fm.copyItem(atPath: src, toPath: dst)
                try fm.removeItem(atPath: src)
                return
            } catch let e as NSError {
                throw FSError.io(e.localizedDescription)
            }
        }
        throw errToFSError(errno: errno)
    }

    public static func waitForPath(_ glob: String, intervalMs: Int = 200, deadlineMs: Int) throws -> StatInfo {
        try FileGuard.ensureAllowed(glob)
        let stop = Double(deadlineMs) / 1000.0
        while Date().timeIntervalSince1970 < stop {
            // Cheap glob: exact-match path is the common case. Future
            // slice can support * / ? wildcards via Glob.swift.
            if let s = try? stat(glob) {
                return s
            }
            Thread.sleep(forTimeInterval: Double(intervalMs) / 1000.0)
        }
        throw FSError.timeout
    }

    public static func download(url urlStr: String, destination: String) throws -> StatInfo {
        try FileGuard.ensureAllowed(destination)
        guard let url = URL(string: urlStr) else { throw FSError.io("invalid URL: \(urlStr)") }
        let tmpURL: URL = try runBlocking {
            try await withCheckedThrowingContinuation { (cont: CheckedContinuation<URL, Error>) in
                URLSession.shared.downloadTask(with: url) { tmp, _, err in
                    if let err = err { cont.resume(throwing: err); return }
                    guard let tmp = tmp else {
                        cont.resume(throwing: FSError.io("download produced no file"))
                        return
                    }
                    // Move out of the cache before the closure returns so
                    // URLSession doesn't reap it.
                    let staged = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent(UUID().uuidString)
                    do {
                        try FileManager.default.moveItem(at: tmp, to: staged)
                        cont.resume(returning: staged)
                    } catch {
                        cont.resume(throwing: error)
                    }
                }.resume()
            }
        }
        // Move into final destination.
        let destURL = URL(fileURLWithPath: destination)
        try? fm.removeItem(at: destURL)
        try fm.moveItem(at: tmpURL, to: destURL)
        return try stat(destination)
    }

    // MARK: directory ops

    public struct DirEntry: Sendable {
        public let name: String
        public let type: FSEntryType
        public let size: Int64
        public let mtime: Double

        public var jsonObject: [String: Any] {
            ["name": name, "type": type.rawValue, "size": size, "mtime": mtime]
        }
    }

    public static func listDirectory(_ path: String) throws -> [DirEntry] {
        try FileGuard.ensureAllowed(path)
        var st = Darwin.stat()
        if sysStat(path, &st) != 0 { throw errToFSError(errno: errno) }
        if entryType(mode: mode_t(st.st_mode)) != .directory { throw FSError.notADirectory }
        let names: [String]
        do {
            names = try fm.contentsOfDirectory(atPath: path)
        } catch let e as NSError {
            throw FSError.io(e.localizedDescription)
        }
        return names.compactMap { name in
            let full = (path as NSString).appendingPathComponent(name)
            var entSt = Darwin.stat()
            guard sysLstat(full, &entSt) == 0 else { return nil }
            return DirEntry(
                name: name,
                type: entryType(mode: mode_t(entSt.st_mode)),
                size: Int64(entSt.st_size),
                mtime: Double(entSt.st_mtimespec.tv_sec) + Double(entSt.st_mtimespec.tv_nsec) / 1e9
            )
        }
    }

    public static func directoryStat(_ path: String) throws -> (count: Int, mtime: Double) {
        // listDirectory + stat both call FileGuard internally; no duplicate
        // check needed here.
        let entries = try listDirectory(path)
        let st = try stat(path)
        return (entries.count, st.mtime)
    }

    public static func createDirectory(_ path: String, withParents: Bool = false) throws {
        try FileGuard.ensureAllowed(path)
        do {
            try fm.createDirectory(atPath: path, withIntermediateDirectories: withParents, attributes: nil)
        } catch let e as NSError {
            switch e.code {
            case NSFileWriteFileExistsError: throw FSError.alreadyExists
            case NSFileWriteNoPermissionError: throw FSError.permissionDenied
            case NSFileNoSuchFileError: throw FSError.notFound
            default: throw FSError.io(e.localizedDescription)
            }
        }
    }

    /// Remove a directory. Defaults to moving the directory (and its
    /// contents) to Trash. `permanent: true` switches to POSIX rmdir / rm
    /// -rf. `recursive` is still required for non-empty directories
    /// regardless of mode — the flag preserves the "yes, I know there's
    /// stuff in here" intent that Trash-recoverability doesn't undo.
    @discardableResult
    public static func removeDirectory(_ path: String, recursive: Bool, permanent: Bool = false) throws -> DeleteOutcome {
        try FileGuard.ensureAllowed(path)

        if !permanent {
            // Trash branch. Enforce the "non-empty needs --recursive" rule
            // in both modes so callers can't accidentally trash a large
            // tree they thought was empty.
            if !recursive {
                let entries = try listDirectory(path)
                if !entries.isEmpty { throw FSError.notEmpty }
            }
            var resultingURL: NSURL?
            do {
                try fm.trashItem(at: URL(fileURLWithPath: path), resultingItemURL: &resultingURL)
                return DeleteOutcome(mode: .trashed, trashURL: resultingURL as URL?)
            } catch let e as NSError {
                switch e.code {
                case NSFileNoSuchFileError, NSFileReadNoSuchFileError: throw FSError.notFound
                case NSFileWriteNoPermissionError: throw FSError.permissionDenied
                default: throw FSError.io(e.localizedDescription)
                }
            }
        }

        // permanent=true → POSIX rmdir / recursive walk.
        if !recursive {
            if rmdir(path) != 0 {
                if errno == ENOTEMPTY { throw FSError.notEmpty }
                throw errToFSError(errno: errno)
            }
            return DeleteOutcome(mode: .permanent, trashURL: nil)
        }
        // Recursive permanent remove — symlinks are NOT traversed (matches
        // windows-modern semantics).
        var st = Darwin.stat()
        if sysLstat(path, &st) != 0 { throw errToFSError(errno: errno) }
        let type = entryType(mode: mode_t(st.st_mode))
        if type == .symlink {
            if unlink(path) != 0 { throw errToFSError(errno: errno) }
            return DeleteOutcome(mode: .permanent, trashURL: nil)
        }
        if type != .directory { throw FSError.notADirectory }
        let entries = try listDirectory(path)
        for entry in entries {
            let child = (path as NSString).appendingPathComponent(entry.name)
            if entry.type == .directory {
                try removeDirectory(child, recursive: true, permanent: true)
            } else {
                if unlink(child) != 0 { throw errToFSError(errno: errno) }
            }
        }
        if rmdir(path) != 0 { throw errToFSError(errno: errno) }
        return DeleteOutcome(mode: .permanent, trashURL: nil)
    }

    // MARK: helpers

    private static func entryType(mode: mode_t) -> FSEntryType {
        let t = mode & S_IFMT
        switch t {
        case S_IFREG: return .file
        case S_IFDIR: return .directory
        case S_IFLNK: return .symlink
        default:      return .other
        }
    }

    private static func statInfo(path: String, st: Darwin.stat) -> StatInfo {
        StatInfo(
            path: path,
            type: entryType(mode: mode_t(st.st_mode)),
            size: Int64(st.st_size),
            mtime: Double(st.st_mtimespec.tv_sec) + Double(st.st_mtimespec.tv_nsec) / 1e9,
            mode: UInt16(st.st_mode) & 0o7777
        )
    }

    private static func errToFSError(errno code: Int32) -> FSError {
        switch code {
        case ENOENT: return .notFound
        case EEXIST: return .alreadyExists
        case ENOTDIR: return .notADirectory
        case ENOTEMPTY: return .notEmpty
        case EACCES, EPERM: return .permissionDenied
        case EXDEV: return .crossDevice
        default:
            let msg = String(cString: strerror(code))
            return .io("errno=\(code) \(msg)")
        }
    }
}

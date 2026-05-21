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

public enum ServerError: Error, CustomStringConvertible {
    case socketFailed(errno: Int32)
    case bindFailed(errno: Int32)
    case listenFailed(errno: Int32)
    case invalidHost(String)

    public var description: String {
        switch self {
        case .socketFailed(let e): return "socket() failed: errno=\(e) (\(String(cString: strerror(e))))"
        case .bindFailed(let e):   return "bind() failed: errno=\(e) (\(String(cString: strerror(e))))"
        case .listenFailed(let e): return "listen() failed: errno=\(e) (\(String(cString: strerror(e))))"
        case .invalidHost(let h):  return "could not parse host as IPv4: \(h)"
        }
    }
}

/// A TCP listener that accepts connections and runs each one on its own
/// dispatch queue. Synchronous BSD sockets — no Network.framework — keep
/// the dependency surface tiny and avoid entitlement requirements at MVP
/// scope.
public final class Server {
    private let host: String
    private let port: UInt16
    private let logger: (String) -> Void
    private var serverFd: Int32 = -1
    private var connectionCounter: Int = 0
    private let acceptQueue = DispatchQueue(label: "rha-mac.accept")

    public init(host: String, port: UInt16, logger: @escaping (String) -> Void) {
        self.host = host
        self.port = port
        self.logger = logger
    }

    /// Bind, listen, and run the accept loop until the process is signalled.
    /// Returns after a graceful shutdown is requested via `stop()`.
    public func run(shouldStop: () -> Bool) throws {
        try bindAndListen()
        logger("listening on \(host):\(port)")
        defer {
            if serverFd >= 0 { close(serverFd) }
        }

        while !shouldStop() {
            var clientAddr = sockaddr_in()
            var addrLen = socklen_t(MemoryLayout<sockaddr_in>.size)
            let clientFd: Int32 = withUnsafeMutablePointer(to: &clientAddr) { ptr -> Int32 in
                ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    Darwin.accept(serverFd, sa, &addrLen)
                }
            }
            if clientFd < 0 {
                if errno == EINTR { continue }
                logger("accept() failed: errno=\(errno) — continuing")
                continue
            }
            connectionCounter += 1
            let label = "conn-\(connectionCounter)"
            DispatchQueue.global(qos: .userInitiated).async { [logger] in
                let session = ConnectionSession(fd: clientFd, label: label, logger: logger)
                session.run()
            }
        }
    }

    private func bindAndListen() throws {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        if fd < 0 { throw ServerError.socketFailed(errno: errno) }
        serverFd = fd

        // SO_REUSEADDR so quick restarts don't trip TIME_WAIT.
        var yes: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, socklen_t(MemoryLayout<Int32>.size))

        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        // inet_pton for the host string.
        let parsed = host.withCString { cstr -> Int32 in
            inet_pton(AF_INET, cstr, &addr.sin_addr)
        }
        if parsed != 1 { throw ServerError.invalidHost(host) }

        let bindRet = withUnsafePointer(to: &addr) { ptr -> Int32 in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                Darwin.bind(fd, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        if bindRet != 0 { throw ServerError.bindFailed(errno: errno) }

        if Darwin.listen(fd, 16) != 0 {
            throw ServerError.listenFailed(errno: errno)
        }
    }
}

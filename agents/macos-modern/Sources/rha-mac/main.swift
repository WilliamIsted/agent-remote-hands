//
// Copyright 2026 William Isted and contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Entry point for the macos-modern v0.3.0 MVP agent.
//
// Responsibilities:
//   - Parse argv into a minimal config (host, port)
//   - Install a SIGINT handler so Ctrl-C ends the accept loop cleanly
//   - Construct the Server and run its accept loop until shutdown
//
// Things NOT in scope for this slice:
//   - Token file generation (gates connection.tier_raise)
//   - Bonjour / mDNS advertisement
//   - launchd integration
//   - ScreenCaptureKit / AX / CGEvent / NSPasteboard — anything verb-side
//

import Foundation
import RemoteHandsCore
#if canImport(Darwin)
import Darwin
#endif

// MARK: argv parsing

struct CLI {
    var host: String = "127.0.0.1"
    var port: UInt16 = 8765
}

func parseArgv(_ argv: [String]) -> CLI {
    var cli = CLI()
    var i = 1
    while i < argv.count {
        let a = argv[i]
        switch a {
        case "--host":
            guard i + 1 < argv.count else { exitUsage("--host requires a value") }
            cli.host = argv[i + 1]
            i += 2
        case "--port":
            guard i + 1 < argv.count else { exitUsage("--port requires a value") }
            guard let p = UInt16(argv[i + 1]) else { exitUsage("--port: invalid value \"\(argv[i + 1])\"") }
            cli.port = p
            i += 2
        case "-h", "--help":
            printUsage()
            exit(0)
        case "--version":
            print("\(AgentIdentity.name) \(AgentIdentity.version) (protocol \(AgentIdentity.protocolVersion), os \(AgentIdentity.osFamily))")
            exit(0)
        default:
            exitUsage("unknown argument: \(a)")
        }
    }
    return cli
}

func printUsage() {
    let exe = "rha-mac"
    print("""
    \(exe) — Agent Remote Hands, macos-modern family (v\(AgentIdentity.version), protocol \(AgentIdentity.protocolVersion))

    Usage:
      \(exe) [--host <addr>] [--port <n>]
      \(exe) --version
      \(exe) --help

    Defaults: --host 127.0.0.1 --port 8765
    """)
}

func exitUsage(_ message: String) -> Never {
    FileHandle.standardError.write(Data("error: \(message)\n\n".utf8))
    printUsage()
    exit(2)
}

// MARK: signal handling

// `nonisolated(unsafe)` because Swift 6 concurrency considers the global
// mutable across actor boundaries; the signal handler is the only writer
// and the accept loop is the only reader, and they synchronise via atomic
// load on the underlying word — fine for this MVP.
nonisolated(unsafe) var shutdownFlag: Int32 = 0

func installSignalHandlers() {
    signal(SIGINT) { _ in
        shutdownFlag = 1
        // Print a newline so the next log line isn't appended after `^C`.
        let msg = "\nshutdown requested\n"
        msg.withCString { ptr in
            _ = Darwin.write(2, ptr, strlen(ptr))
        }
    }
    signal(SIGTERM) { _ in
        shutdownFlag = 1
    }
    // Ignore SIGPIPE — peer dropping mid-write should produce EPIPE on the
    // write() return path, not kill the process.
    signal(SIGPIPE, SIG_IGN)
}

// MARK: bootstrap

let cli = parseArgv(CommandLine.arguments)
installSignalHandlers()

func log(_ message: String) {
    let iso = ISO8601DateFormatter().string(from: Date())
    let line = "\(iso) \(message)\n"
    FileHandle.standardError.write(Data(line.utf8))
}

log("\(AgentIdentity.name) \(AgentIdentity.version) starting (\(AgentIdentity.osFamily))")
let server = Server(host: cli.host, port: cli.port, logger: log)

do {
    try server.run(shouldStop: { shutdownFlag != 0 })
} catch {
    log("server error: \(error)")
    exit(1)
}

log("shutdown complete")

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
    /// Bonjour advertising. Default-on (matches the Windows-modern
    /// convention from ldoc-104-mdns-default-on-no-discoverable); pass
    /// --no-discoverable to opt out.
    var discoverable: Bool = true
    /// Gate on `system.power.{shutdown,reboot,logoff,sleep,hibernate}`.
    /// Default OFF so a remote session — including the conformance suite
    /// — cannot shut down the host machine. Operator must explicitly
    /// pass `--allow-power-state-changes` to enable.
    var allowPowerStateChanges: Bool = false
    /// Default endpoint for vision.describe/calibrate when the verb is
    /// called without an --endpoint arg. Empty = no default. Precedence:
    /// --vision-endpoint flag > REMOTE_HANDS_VISION_ENDPOINT env > empty.
    var visionEndpoint: String = ""
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
        case "--no-discoverable":
            cli.discoverable = false
            i += 1
        case "--allow-power-state-changes":
            cli.allowPowerStateChanges = true
            i += 1
        case "--vision-endpoint":
            guard i + 1 < argv.count else { exitUsage("--vision-endpoint requires a value") }
            cli.visionEndpoint = argv[i + 1]
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
    // CLI flag wins; fall back to the environment variable.
    if cli.visionEndpoint.isEmpty,
       let env = ProcessInfo.processInfo.environment["REMOTE_HANDS_VISION_ENDPOINT"],
       !env.isEmpty {
        cli.visionEndpoint = env
    }
    return cli
}

func printUsage() {
    let exe = "rha-mac"
    print("""
    \(exe) — Agent Remote Hands, macos-modern family (v\(AgentIdentity.version), protocol \(AgentIdentity.protocolVersion))

    Usage:
      \(exe) [--host <addr>] [--port <n>] [--no-discoverable] [--allow-power-state-changes]
            [--vision-endpoint <url>]
      \(exe) --version
      \(exe) --help

    Defaults:
      --host 127.0.0.1, --port 8765, Bonjour advertising on, power state
      changes (shutdown/reboot/logoff/sleep/hibernate) DISABLED. Pass
      --allow-power-state-changes to opt those verbs in.
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

// libc's `signal()` installs handlers with SA_RESTART by default on Darwin,
// so a blocked `accept(2)` resumes after the signal handler returns instead
// of failing with EINTR — which means SIGTERM doesn't actually stop the
// accept loop until a connection happens to arrive. We use `sigaction(2)`
// with `sa_flags = 0` (no SA_RESTART) so signals interrupt the syscall and
// the loop can check `shutdownFlag` promptly.

private let shutdownHandler: @convention(c) (Int32) -> Void = { _ in
    shutdownFlag = 1
    let msg = "\nshutdown requested\n"
    msg.withCString { ptr in
        _ = Darwin.write(2, ptr, strlen(ptr))
    }
}

func installSignalHandlers() {
    var act = sigaction()
    act.__sigaction_u.__sa_handler = shutdownHandler
    act.sa_flags = 0
    sigemptyset(&act.sa_mask)
    sigaction(SIGINT, &act, nil)
    sigaction(SIGTERM, &act, nil)

    // Ignore SIGPIPE so peer-dropped writes surface as EPIPE on the return
    // path rather than killing the process.
    var ignore = sigaction()
    ignore.__sigaction_u.__sa_handler = SIG_IGN
    ignore.sa_flags = 0
    sigemptyset(&ignore.sa_mask)
    sigaction(SIGPIPE, &ignore, nil)
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

// Apply the power-policy gate before the server starts accepting verbs.
PowerPolicy.allowStateChanges = cli.allowPowerStateChanges
if cli.allowPowerStateChanges {
    log("WARNING: --allow-power-state-changes is set — system.power.{shutdown,reboot,logoff,sleep,hibernate} will execute on the host")
} else {
    log("system.power.{shutdown,reboot,logoff,sleep,hibernate} are DISABLED (default); pass --allow-power-state-changes to enable")
}

// Publish the vision-LLM endpoint default before the server starts.
VisionConfig.defaultEndpoint = cli.visionEndpoint
if cli.visionEndpoint.isEmpty {
    log("vision.describe/calibrate: no default endpoint (pass --vision-endpoint or set REMOTE_HANDS_VISION_ENDPOINT, or supply --endpoint per call)")
} else {
    log("vision.describe/calibrate default endpoint: \(cli.visionEndpoint)")
}

// Trigger the macOS TCC consent dialogs (Screen Recording, Accessibility,
// Input Monitoring). macOS shows each only when the permission is
// undetermined, so in practice this prompts once, on first run.
Permissions.requestAll(logger: log)

let tokenStore: TokenStore?
do {
    let store = try TokenStore.initialise()
    log("token file written to \(store.path) (token rotates on each restart)")
    tokenStore = store
} catch {
    log("token initialisation failed: \(error) — tier_raise will return not_supported_by_target")
    tokenStore = nil
}

let mdns: MDNSResponder?
if cli.discoverable {
    let r = MDNSResponder(port: cli.port, logger: log)
    r.start()
    mdns = r
} else {
    log("Bonjour advertising disabled (--no-discoverable)")
    mdns = nil
}

let server = Server(host: cli.host, port: cli.port, tokenStore: tokenStore, logger: log)

do {
    try server.run(shouldStop: { shutdownFlag != 0 })
} catch {
    log("server error: \(error)")
    mdns?.stop()
    exit(1)
}

mdns?.stop()
log("shutdown complete")

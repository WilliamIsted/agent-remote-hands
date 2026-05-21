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

/// Bonjour / mDNS responder for `_remote-hands._tcp.local.`. macOS ships
/// the system `mDNSResponder`; this class just registers a service entry
/// with it via `NetService`. TXT record advertises os family, agent
/// version, and protocol version so controllers can filter on the wire.
///
/// macOS analogue of `agents/windows-modern/src/mdns.cpp` — but there's no
/// hand-rolled UDP 5353 listener needed, the system service handles all
/// of that.
public final class MDNSResponder: @unchecked Sendable {
    private let service: NetService
    private let logger: (String) -> Void
    private var advertiseThread: Thread?

    public init(port: UInt16, logger: @escaping (String) -> Void) {
        // Empty name → mDNSResponder uses the computer's sharing name.
        let svc = NetService(
            domain: "local.",
            type: "_remote-hands._tcp.",
            name: "",
            port: Int32(port)
        )
        let txt: [String: Data] = [
            "os": Data(AgentIdentity.osFamily.utf8),
            "version": Data(AgentIdentity.version.utf8),
            "protocol": Data(AgentIdentity.protocolVersion.utf8),
        ]
        svc.setTXTRecord(NetService.data(fromTXTRecord: txt))
        self.service = svc
        self.logger = logger
    }

    /// Publish the service. Runs on a dedicated thread with its own
    /// RunLoop — NetService requires a run loop to deliver delegate
    /// callbacks and dispatch socket activity, and the main accept loop
    /// uses blocking BSD sockets so we can't share its thread.
    public func start() {
        let svc = service
        let log = logger
        let t = Thread {
            svc.schedule(in: .current, forMode: .default)
            svc.publish()
            log("Bonjour advertiser publishing _remote-hands._tcp.local.")
            // Drive the run loop until the thread is asked to exit.
            while !Thread.current.isCancelled {
                _ = RunLoop.current.run(mode: .default, before: Date(timeIntervalSinceNow: 1.0))
            }
            svc.stop()
        }
        t.name = "rha-mac.mdns"
        t.start()
        advertiseThread = t
    }

    /// Stop the responder. The thread checks `isCancelled` every second so
    /// shutdown takes up to a second to take effect.
    public func stop() {
        advertiseThread?.cancel()
        advertiseThread = nil
    }
}

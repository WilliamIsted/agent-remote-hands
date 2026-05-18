//   Copyright 2026 William Isted and contributors
//
//   Licensed under the Apache License, Version 2.0 (the "License");
//   you may not use this file except in compliance with the License.
//   You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.

#pragma once

// Minimal mDNS responder for opt-in LAN discovery.
//
// Advertises the agent at `_remote-hands._tcp.local.` on UDP 5353. On each
// inbound query whose payload contains the service name, we reply with the
// canonical PTR / SRV / TXT / A record set described in PROTOCOL.md §8.
//
// This is deliberately a "good enough" responder — it doesn't probe for
// instance-name conflicts, doesn't send unsolicited announcements at start,
// and doesn't track legacy unicast queries. A LAN with a known agent count
// and trusted clients is the assumed deployment.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace remote_hands::mdns {

struct Config {
    std::uint16_t   tcp_port    = 8765;        // The agent's listening port
    std::string     os_tag      = "windows-modern";
};

class Responder {
public:
    explicit Responder(Config cfg);
    ~Responder();
    Responder(const Responder&)            = delete;
    Responder& operator=(const Responder&) = delete;

    // Spawns the responder thread. Idempotent.
    void start();

    // Signals the thread to exit and joins it. Idempotent.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace remote_hands::mdns

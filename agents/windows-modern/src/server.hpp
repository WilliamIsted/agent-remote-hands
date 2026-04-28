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

#include "config.hpp"

#include <functional>
#include <memory>

namespace remote_hands {

// TCP listener + connection accept loop.
//
// Construction binds and listens on the configured port. run() blocks until
// the supplied predicate returns true (set the flag from a signal handler).
//
// Each accepted connection is handed off to a per-connection thread driving
// the connection state machine (see connection.hpp, added in build phase 3).
class Server {
public:
    explicit Server(const Config& config);
    ~Server();
    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    // Runs the accept loop. Returns when shutdown_requested() returns true
    // or on a fatal accept error. Blocks the calling thread.
    void run(std::function<bool()> shutdown_requested);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace remote_hands

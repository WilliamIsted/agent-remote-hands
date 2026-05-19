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

// Socket-state watchdog for windows-modern and windows-legacy.
//
// A background thread wakes every interval_ms milliseconds and probes the
// listen socket via getsockopt(SO_ERROR). If the socket appears dead (e.g.
// after a suspend/resume cycle where the power-broadcast path failed), it
// invokes the on_dead callback — which should trigger a rebind.
//
// Silent on a healthy socket; logs only on dead + rebind.

#include <functional>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>

namespace remote_hands {

// Start the socket-state watchdog thread.
// on_dead is called (from the watchdog thread) when the listen socket
// appears dead. The callback should trigger server_rebind().
// interval_ms: probe interval in milliseconds (default 30 000).
void watchdog_start(std::function<void()> on_dead,
                    unsigned interval_ms = 30'000);

// Update the socket the watchdog monitors. Thread-safe.
// Call after each successful rebind with the new socket handle,
// and once at startup with the initial socket.
void watchdog_update_socket(SOCKET s);

// Signal the watchdog thread to stop. Call from the server destructor.
void watchdog_stop();

}  // namespace remote_hands

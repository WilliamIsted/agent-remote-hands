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

// Wire-format reader / writer per PROTOCOL.md §1.
//
// Framing rules (v2.1):
//   Header line:  <directive> <args...>\n          (UTF-8, max 65 535 bytes)
//   Payload:      exactly <length> bytes following the header (when grammar
//                 specifies a length argument as the final positional arg).
//
// Header tokens are space-separated, with optional double-quote grouping
// for tokens that contain spaces — see PROTOCOL.md §1.2.5. Quoted tokens
// have no escape mechanism inside the quotes, so embedded `"` is not
// representable on the header line; verbs that need raw bytes use the
// length-prefixed payload form.
//
// The Reader/Writer pair are framing-layer primitives. They do not interpret
// verb semantics; that lives in the connection state machine and the per-verb
// handlers. A consequence: the framing layer doesn't know which arg is the
// payload-length. Callers that issue payload-bearing verbs read the header,
// pull the last arg as the length, then call read_payload().

#include "errors.hpp"

#include <cstddef>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>

namespace remote_hands::wire {

constexpr std::size_t kMaxHeaderLineBytes = 65535;

struct Request {
    std::string                 verb;   // e.g. "system.info"
    std::vector<std::string>    args;   // tokens after the verb (quotes already stripped)
    // If the header could not be tokenised (e.g. unmatched quote), the
    // dispatcher emits ERR invalid_args with this message instead of
    // attempting to dispatch.
    std::string                 parse_error;
};

// Tokenises a header line (with any trailing \r already stripped) per
// PROTOCOL.md §1.2.5. Tokens are space-separated; tokens enclosed in
// `"..."` are taken literally (including spaces and backslashes; embedded
// `"` is not representable). Empty tokens (runs of spaces) are skipped;
// `""` represents an explicit empty arg.
//
// On a malformed header (currently: unmatched opening quote), the returned
// Request has a non-empty parse_error and an empty verb; the dispatcher is
// responsible for surfacing this as ERR invalid_args.
//
// Exposed publicly so unit tests can exercise it without spinning up a
// socket.
Request tokenize_header(std::string_view line);

// Reads framed messages from a connected TCP socket.
// Not thread-safe (one reader per connection thread).
class Reader {
public:
    explicit Reader(SOCKET socket);

    // Reads the next request header line. Returns nullopt on clean EOF.
    // Throws std::runtime_error on socket error or malformed header.
    std::optional<Request> read_header();

    // Reads exactly `length` bytes of payload from the wire (drawing first
    // from the internal buffer left over after read_header()).
    // Throws on short read or socket error.
    std::vector<std::byte> read_payload(std::size_t length);

    // Discards any buffered data without disturbing the socket. Used to
    // implement `connection.reset` recovery.
    void flush_buffer() noexcept { buffer_.clear(); }

private:
    SOCKET                  socket_;
    std::vector<std::byte>  buffer_;    // bytes received but not yet consumed

    // Pulls more bytes from the socket into buffer_. Returns false on EOF.
    bool fill_from_socket();

    // If buffer_ contains a complete \n-terminated line, returns it (without
    // the trailing \n, with any trailing \r stripped). Otherwise nullopt.
    std::optional<std::string> try_extract_line();
};

// Writes framed responses + EVENT frames to a connected TCP socket.
// Thread-safe: a single Writer instance is shared between the verb-handler
// thread and any background subscription threads emitting EVENT frames.
class Writer {
public:
    explicit Writer(SOCKET socket);

    // Success responses.
    void write_ok();                                                    // "OK 0\n"
    void write_ok(std::span<const std::byte> payload);                  // "OK <len>\n<bytes>"
    void write_ok(std::string_view payload);                            // utf-8 convenience

    // Error responses. detail_json is the raw JSON body (caller-formatted).
    void write_err(ErrorCode code);
    void write_err(ErrorCode code, std::string_view detail_json);

    // Asynchronous event for an active subscription.
    void write_event(std::string_view subscription_id,
                     std::span<const std::byte> payload);
    void write_event(std::string_view subscription_id,
                     std::string_view payload);

private:
    void write_raw(std::span<const std::byte> bytes);
    void write_raw(std::string_view sv);

    SOCKET      socket_;
    std::mutex  mutex_;
};

}  // namespace remote_hands::wire

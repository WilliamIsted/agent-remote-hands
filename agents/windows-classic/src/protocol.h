/*   Copyright 2026 William Isted and contributors
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

/* ARH v2.1 wire framing, hand-ported to C89 from agents/shared/protocol.cpp.
 *
 * Framing (PROTOCOL.md 1.2): every message is one header line terminated by
 * '\n' (a leading '\r' before '\n' is tolerated and stripped) optionally
 * followed by exactly <length> payload bytes when the directive grammar ends
 * in a <length> arg. Responses are uniform:
 *
 *     OK 0\n                       success, no body
 *     OK <len>\n<body>             success with body
 *     ERR <code> 0\n               error, no detail
 *     ERR <code> <len>\n<json>     error with JSON detail
 *
 * NOTE (deviation from the W8 prompt sketch): the prompt described an
 * "OK\r\nContent-Length: N\r\n\r\n" MIME-style frame and a "HELLO rha/2.1"
 * handshake line. The real contract -- tests/conformance/wire.py and
 * PROTOCOL.md 1.2 / 2.2 -- uses the single-line length-prefixed form above
 * and makes the handshake the ordinary verb `connection.hello <name> <ver>`.
 * The conformance suite is the contract (repo CLAUDE.md), so this port
 * follows the suite, not the sketch. */

#ifndef RH_PROTOCOL_H
#define RH_PROTOCOL_H

#include "types.h"

#include <winsock.h>

#define RH_PROTO_OK    0
#define RH_PROTO_EOF  (-1)   /* peer closed cleanly between frames */
#define RH_PROTO_ERR  (-2)   /* fatal wire error: drop the connection */

typedef struct {
    SOCKET sock;
    char*  buf;     /* heap buffer, RH_MAX_HEADER_LEN + 1 bytes */
    int    have;    /* bytes currently buffered                 */
} RhReader;

typedef struct {
    char        verb[RH_MAX_VERB_LEN];
    char        args[RH_MAX_ARGS][RH_MAX_ARG_LEN];
    int         argc;
    int         parse_error;     /* 1 => malformed header (e.g. bad quote) */
    const char* parse_message;   /* human message for ERR invalid_args     */
} RhRequest;

int  rh_reader_init(RhReader* r, SOCKET s);
void rh_reader_free(RhReader* r);
void rh_reader_flush(RhReader* r);   /* connection.reset: discard buffer */

/* Read + tokenize one request. Returns RH_PROTO_OK / RH_PROTO_EOF /
 * RH_PROTO_ERR. On OK, *req is populated (req->parse_error may be set when
 * the line framed cleanly but the args were malformed -- the caller emits
 * ERR invalid_args in that case, per PROTOCOL.md 1.2.5). */
int  rh_read_request(RhReader* r, RhRequest* req);

/* Pure tokenizer (PROTOCOL.md 1.2.5). Exposed for clarity/testing. */
void rh_tokenize(const char* line, RhRequest* req);

/* Read exactly `n` length-prefixed payload bytes that follow a header whose
 * grammar ends in <length> (file.write, clipboard.set, input.type, ...).
 * Drains the reader's leftover buffer first, then pulls the rest off the
 * socket. Returns RH_PROTO_OK / RH_PROTO_EOF / RH_PROTO_ERR. W8 only read
 * header lines; W9 verbs that take a payload need this. */
int  rh_read_payload(RhReader* r, char* out, int n);

/* Response writers (best-effort; wire errors surface on the next recv). */
void rh_send_ok(SOCKET s);
void rh_send_ok_json(SOCKET s, const char* json);
/* Length-explicit OK body: the payload may contain NUL bytes (screen.capture
 * returns raw PNG/BMP), so this cannot go through the strlen-based
 * rh_send_ok_json. */
void rh_send_ok_bytes(SOCKET s, const char* data, int len);
void rh_send_err(SOCKET s, const char* code);
void rh_send_err_json(SOCKET s, const char* code, const char* json);

#endif /* RH_PROTOCOL_H */

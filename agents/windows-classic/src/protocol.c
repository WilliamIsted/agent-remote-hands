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

#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RH_READ_CAP  (RH_MAX_HEADER_LEN + 1)

/* --- low-level socket I/O ---------------------------------------------- */

static void send_all(SOCKET s, const char* p, int n)
{
    int sent;
    while (n > 0) {
        sent = send(s, p, n, 0);
        if (sent <= 0) {
            return;   /* best-effort; the next recv() will see the break */
        }
        p += sent;
        n -= sent;
    }
}

/* --- tokenizer (PROTOCOL.md 1.2.5) ------------------------------------- */

void rh_tokenize(const char* line, RhRequest* req)
{
    int n;
    int i;
    int start;
    int got_verb;

    req->verb[0] = '\0';
    req->argc = 0;
    req->parse_error = 0;
    req->parse_message = NULL;

    n = (int)strlen(line);
    i = 0;
    got_verb = RH_FALSE;

    while (i < n) {
        int   tok_len;
        char* dst;
        int   dst_cap;

        /* Skip run of spaces. */
        while (i < n && line[i] == ' ') {
            ++i;
        }
        if (i >= n) {
            break;
        }

        if (line[i] == '"') {
            ++i;                       /* consume opening quote */
            start = i;
            while (i < n && line[i] != '"') {
                ++i;
            }
            if (i >= n) {
                /* Unmatched opening quote: the line framed fine, only the
                 * args are malformed. Caller emits ERR invalid_args. */
                req->verb[0] = '\0';
                req->argc = 0;
                req->parse_error = 1;
                req->parse_message = "unmatched quote in header";
                return;
            }
            tok_len = i - start;
            ++i;                       /* consume closing quote */
        } else {
            start = i;
            while (i < n && line[i] != ' ') {
                ++i;
            }
            tok_len = i - start;
        }

        if (!got_verb) {
            dst = req->verb;
            dst_cap = RH_MAX_VERB_LEN;
            got_verb = RH_TRUE;
        } else if (req->argc < RH_MAX_ARGS) {
            dst = req->args[req->argc];
            dst_cap = RH_MAX_ARG_LEN;
            req->argc += 1;
        } else {
            continue;                  /* silently drop overflow args */
        }

        if (tok_len > dst_cap - 1) {
            tok_len = dst_cap - 1;     /* truncate over-long token */
        }
        memcpy(dst, line + start, (size_t)tok_len);
        dst[tok_len] = '\0';
    }
}

/* --- reader ------------------------------------------------------------ */

int rh_reader_init(RhReader* r, SOCKET s)
{
    r->sock = s;
    r->have = 0;
    r->buf = (char*)malloc(RH_READ_CAP);
    if (r->buf == NULL) {
        return RH_PROTO_ERR;
    }
    return RH_PROTO_OK;
}

void rh_reader_free(RhReader* r)
{
    if (r->buf != NULL) {
        free(r->buf);
        r->buf = NULL;
    }
    r->have = 0;
}

void rh_reader_flush(RhReader* r)
{
    r->have = 0;
}

int rh_read_request(RhReader* r, RhRequest* req)
{
    int   i;
    int   line_len;
    int   n;
    char* nl;

    for (;;) {
        /* Look for a complete line in the buffer. */
        nl = NULL;
        for (i = 0; i < r->have; ++i) {
            if (r->buf[i] == '\n') {
                nl = r->buf + i;
                break;
            }
        }

        if (nl != NULL) {
            line_len = (int)(nl - r->buf);
            if (line_len > 0 && r->buf[line_len - 1] == '\r') {
                line_len -= 1;       /* tolerate CRLF */
            }
            r->buf[line_len] = '\0';
            rh_tokenize(r->buf, req);

            /* Drop the consumed line + '\n' from the buffer. */
            {
                int consumed = i + 1;
                int rest = r->have - consumed;
                if (rest > 0) {
                    memmove(r->buf, r->buf + consumed, (size_t)rest);
                }
                r->have = rest;
            }
            return RH_PROTO_OK;
        }

        if (r->have >= RH_MAX_HEADER_LEN) {
            return RH_PROTO_ERR;     /* header exceeds 65535: drop */
        }

        n = recv(r->sock, r->buf + r->have, RH_READ_CAP - r->have, 0);
        if (n == 0) {
            return (r->have == 0) ? RH_PROTO_EOF : RH_PROTO_ERR;
        }
        if (n < 0) {
            return RH_PROTO_ERR;
        }
        r->have += n;
    }
}

/* --- length-prefixed payload reader (W9) ------------------------------- */

int rh_read_payload(RhReader* r, char* out, int n)
{
    int copied;
    int take;
    int got;

    if (n < 0) {
        return RH_PROTO_ERR;
    }
    if (n == 0) {
        return RH_PROTO_OK;
    }

    copied = 0;

    /* Bytes that arrived in the same recv() as the header line are still
     * sitting in the reader buffer -- consume those first. */
    if (r->have > 0) {
        take = (r->have < n) ? r->have : n;
        memcpy(out, r->buf, (size_t)take);
        if (r->have > take) {
            memmove(r->buf, r->buf + take, (size_t)(r->have - take));
        }
        r->have -= take;
        copied = take;
    }

    /* Pull the remainder straight off the socket. */
    while (copied < n) {
        got = recv(r->sock, out + copied, n - copied, 0);
        if (got == 0) {
            return RH_PROTO_EOF;
        }
        if (got < 0) {
            return RH_PROTO_ERR;
        }
        copied += got;
    }
    return RH_PROTO_OK;
}

/* --- response writers -------------------------------------------------- */

void rh_send_ok(SOCKET s)
{
    send_all(s, "OK 0\n", 5);
}

void rh_send_ok_bytes(SOCKET s, const char* data, int len)
{
    char header[32];
    int  hn;

    if (data == NULL || len <= 0) {
        rh_send_ok(s);
        return;
    }
    hn = _snprintf(header, sizeof(header), "OK %d\n", len);
    if (hn <= 0 || hn >= (int)sizeof(header)) {
        rh_send_err(s, "wire_desync");
        return;
    }
    send_all(s, header, hn);
    send_all(s, data, len);
}

void rh_send_ok_json(SOCKET s, const char* json)
{
    char header[32];
    int  body_len;
    int  hn;

    if (json == NULL) {
        rh_send_ok(s);
        return;
    }
    body_len = (int)strlen(json);
    hn = _snprintf(header, sizeof(header), "OK %d\n", body_len);
    if (hn <= 0 || hn >= (int)sizeof(header)) {
        rh_send_err(s, "wire_desync");
        return;
    }
    send_all(s, header, hn);
    send_all(s, json, body_len);
}

void rh_send_err(SOCKET s, const char* code)
{
    char line[96];
    int  hn;

    hn = _snprintf(line, sizeof(line), "ERR %s 0\n", code);
    if (hn <= 0 || hn >= (int)sizeof(line)) {
        send_all(s, "ERR wire_desync 0\n", 18);
        return;
    }
    send_all(s, line, hn);
}

void rh_send_err_json(SOCKET s, const char* code, const char* json)
{
    char header[128];
    int  body_len;
    int  hn;

    if (json == NULL || json[0] == '\0') {
        rh_send_err(s, code);
        return;
    }
    body_len = (int)strlen(json);
    hn = _snprintf(header, sizeof(header), "ERR %s %d\n", code, body_len);
    if (hn <= 0 || hn >= (int)sizeof(header)) {
        rh_send_err(s, code);
        return;
    }
    send_all(s, header, hn);
    send_all(s, json, body_len);
}

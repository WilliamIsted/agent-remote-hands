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

#include "clipboard.h"
#include "common.h"
#include "../protocol.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#define RH_CLIP_MAX (1 * 1024 * 1024)

void rh_verb_clipboard_get(RhConn* c, const RhRequest* req)
{
    HANDLE h;
    char*  text;
    int    len;

    (void)req;

    if (!OpenClipboard(NULL)) {
        rh_ok(c);                 /* no access -> treat as empty */
        return;
    }
    if (!IsClipboardFormatAvailable(CF_TEXT)) {
        CloseClipboard();
        rh_ok(c);                 /* no text -> empty payload */
        return;
    }
    h = GetClipboardData(CF_TEXT);
    if (h == NULL) {
        CloseClipboard();
        rh_ok(c);
        return;
    }
    text = (char*)GlobalLock(h);
    if (text == NULL) {
        CloseClipboard();
        rh_ok(c);
        return;
    }
    len = lstrlenA(text);
    rh_ok_bytes(c, text, len);
    GlobalUnlock(h);
    CloseClipboard();
}

void rh_verb_clipboard_set(RhConn* c, const RhRequest* req)
{
    RhArgs  a;
    int     n;
    char*   body;
    int     rc;
    HGLOBAL hg;
    char*   dst;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "clipboard.set requires <length>");
        return;
    }
    n = atoi(a.pos[0]);
    if (n < 0 || n > RH_CLIP_MAX) {
        rh_err_msg(c, "invalid_args", "bad payload length");
        return;
    }
    body = (char*)malloc((size_t)(n > 0 ? n : 1));
    if (body == NULL) {
        rh_err(c, "wire_desync");
        return;
    }
    if (n > 0) {
        rc = rh_read_payload(&c->reader, body, n);
        if (rc != RH_PROTO_OK) {
            free(body);
            rh_err(c, "wire_desync");
            return;
        }
    }

    if (!OpenClipboard(NULL)) {
        free(body);
        rh_err_kv(c, "lock_held", "lock_type", "clipboard");
        return;
    }
    EmptyClipboard();

    hg = GlobalAlloc(GMEM_MOVEABLE, (DWORD)(n + 1));
    if (hg == NULL) {
        CloseClipboard();
        free(body);
        rh_err(c, "wire_desync");
        return;
    }
    dst = (char*)GlobalLock(hg);
    if (dst == NULL) {
        GlobalFree(hg);
        CloseClipboard();
        free(body);
        rh_err(c, "wire_desync");
        return;
    }
    if (n > 0) {
        memcpy(dst, body, (size_t)n);
    }
    dst[n] = '\0';
    GlobalUnlock(hg);

    /* Ownership of hg transfers to the clipboard on success. */
    SetClipboardData(CF_TEXT, hg);
    CloseClipboard();
    free(body);
    rh_ok(c);
}

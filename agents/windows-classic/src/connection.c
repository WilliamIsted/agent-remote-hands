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

#include "connection.h"
#include "debug.h"
#include "json.h"
#include "token.h"
#include "verbs/system.h"
#include "verbs/screen.h"
#include "verbs/window.h"
#include "verbs/input.h"
#include "verbs/file.h"
#include "verbs/directory.h"
#include "verbs/process.h"
#include "verbs/registry.h"
#include "verbs/clipboard.h"
#include "verbs/watch.h"
#include "verbs/system_power.h"

#include <stdio.h>
#include <string.h>

#include <winsock.h>

/* --- tier vocabulary --------------------------------------------------- */

const char* rh_tier_name(int tier)
{
    switch (tier) {
        case RH_TIER_READ:        return "read";
        case RH_TIER_CREATE:      return "create";
        case RH_TIER_UPDATE:      return "update";
        case RH_TIER_DELETE:      return "delete";
        case RH_TIER_EXTRA_RISKY: return "extra_risky";
        default:                  return NULL;
    }
}

int rh_tier_from_name(const char* s)
{
    if (s == NULL)                       return -1;
    if (strcmp(s, "read")        == 0)   return RH_TIER_READ;
    if (strcmp(s, "create")      == 0)   return RH_TIER_CREATE;
    if (strcmp(s, "update")      == 0)   return RH_TIER_UPDATE;
    if (strcmp(s, "delete")      == 0)   return RH_TIER_DELETE;
    if (strcmp(s, "extra_risky") == 0)   return RH_TIER_EXTRA_RISKY;
    /* v2.0 names (observe/drive/power) are intentionally unknown -> the
     * caller maps that to ERR invalid_args per PROTOCOL.md 2.3. */
    return -1;
}

/* --- small JSON-detail emitters ---------------------------------------- */

static void err_kv(SOCKET s, const char* code,
                    const char* k, const char* v)
{
    RhJson      j;
    const char* r;

    rh_json_init(&j);
    rh_json_begin_obj(&j);
    rh_json_key(&j, k);
    rh_json_str(&j, v);
    rh_json_end_obj(&j);
    r = rh_json_finish(&j);
    if (r != NULL) {
        rh_send_err_json(s, code, r);
    } else {
        rh_send_err(s, code);
    }
}

static void err_kv2(SOCKET s, const char* code,
                     const char* k1, const char* v1,
                     const char* k2, const char* v2)
{
    RhJson      j;
    const char* r;

    rh_json_init(&j);
    rh_json_begin_obj(&j);
    rh_json_key(&j, k1);
    rh_json_str(&j, v1);
    rh_json_key(&j, k2);
    rh_json_str(&j, v2);
    rh_json_end_obj(&j);
    r = rh_json_finish(&j);
    if (r != NULL) {
        rh_send_err_json(s, code, r);
    } else {
        rh_send_err(s, code);
    }
}

static void ok_kv(SOCKET s, const char* k, const char* v)
{
    RhJson      j;
    const char* r;

    rh_json_init(&j);
    rh_json_begin_obj(&j);
    rh_json_key(&j, k);
    rh_json_str(&j, v);
    rh_json_end_obj(&j);
    r = rh_json_finish(&j);
    if (r != NULL) {
        rh_send_ok_json(s, r);
    } else {
        rh_send_err(s, "wire_desync");
    }
}

/* --- connection.hello version negotiation ------------------------------ */

/* Classic is pinned to protocol 2.0 / 2.1 (D8): accept "2", "2.0*", "2.1*";
 * reject "2.2"+ and any non-2 major. A v2.2 hello therefore yields
 * ERR protocol_mismatch -- the behaviour D8 requires. */
static int version_classic_ok(const char* v)
{
    char minor[8];
    int  i;
    int  mi;

    if (v == NULL)              return RH_FALSE;
    if (strcmp(v, "2") == 0)    return RH_TRUE;
    if (v[0] != '2' || v[1] != '.') return RH_FALSE;

    i  = 2;
    mi = 0;
    while (v[i] != '\0' && v[i] != '.' && mi < (int)sizeof(minor) - 1) {
        minor[mi++] = v[i++];
    }
    minor[mi] = '\0';
    if (strcmp(minor, "0") == 0 || strcmp(minor, "1") == 0) {
        return RH_TRUE;
    }
    return RH_FALSE;
}

/* --- verb table -------------------------------------------------------- */

/* (deviation from the W9 prompt sketch) The W8 handler signature was
 * void(*)(RhConn*) -- no request access -- which is sufficient only for
 * the three argument-free system.* verbs. Every W9 OS verb needs the
 * tokenised args and (for file.write / clipboard.set / input.type) the
 * length-prefixed payload, so the handler signature is widened to
 * (RhConn*, const RhRequest*). The dispatch() tier gate already had `req`
 * in scope; this just threads it through. Tiers follow PROTOCOL.md §4
 * CRUDX letters (R/C/U/D/X -> READ/CREATE/UPDATE/DELETE/EXTRA_RISKY).
 *
 * D9: element.* (14), watch.element are deliberately ABSENT -- the default
 * dispatch path returns ERR not_supported for them, and the conformance
 * suite's needs_verb() skips their tests. system.* power verbs are likewise
 * unimplemented on classic (needs_verb-gated) and intentionally absent. */

typedef struct {
    const char* verb;
    int         tier;
    void        (*fn)(RhConn*, const RhRequest*);
} VerbEntry;

static const VerbEntry kVerbs[] = {
    /* system.* */
    { "system.info",               RH_TIER_READ,        rh_verb_system_info         },
    { "system.capabilities",       RH_TIER_READ,        rh_verb_system_capabilities },
    { "system.health",             RH_TIER_READ,        rh_verb_system_health       },

    /* system.power.* */
    { "system.power.blockers",     RH_TIER_READ,        rh_verb_power_blockers  },
    { "system.power.lock",         RH_TIER_READ,        rh_verb_power_lock      },
    { "system.power.reboot",       RH_TIER_EXTRA_RISKY, rh_verb_power_reboot    },
    { "system.power.shutdown",     RH_TIER_EXTRA_RISKY, rh_verb_power_shutdown  },
    { "system.power.logoff",       RH_TIER_EXTRA_RISKY, rh_verb_power_logoff    },
    { "system.power.hibernate",    RH_TIER_EXTRA_RISKY, rh_verb_power_hibernate },
    { "system.power.sleep",        RH_TIER_EXTRA_RISKY, rh_verb_power_sleep     },
    { "system.power.cancel",       RH_TIER_EXTRA_RISKY, rh_verb_power_cancel    },

    /* screen.* */
    { "screen.capture",      RH_TIER_READ,        rh_verb_screen_capture      },

    /* window.* */
    { "window.list",         RH_TIER_READ,        rh_verb_window_list         },
    { "window.find",         RH_TIER_READ,        rh_verb_window_find         },
    { "window.focus",        RH_TIER_UPDATE,      rh_verb_window_focus        },
    { "window.close",        RH_TIER_UPDATE,      rh_verb_window_close        },
    { "window.move",         RH_TIER_UPDATE,      rh_verb_window_move         },
    { "window.state",        RH_TIER_READ,        rh_verb_window_state        },

    /* input.* (all update tier) */
    { "input.click",         RH_TIER_UPDATE,      rh_verb_input_click         },
    { "input.move",          RH_TIER_UPDATE,      rh_verb_input_move          },
    { "input.scroll",        RH_TIER_UPDATE,      rh_verb_input_scroll        },
    { "input.key",           RH_TIER_UPDATE,      rh_verb_input_key           },
    { "input.type",          RH_TIER_UPDATE,      rh_verb_input_type          },
    { "input.send_message",  RH_TIER_UPDATE,      rh_verb_input_send_message  },
    { "input.post_message",  RH_TIER_UPDATE,      rh_verb_input_post_message  },

    /* input.* v2.1 namespace additions */
    { "input.position",          RH_TIER_READ,   rh_verb_input_position          },
    { "input.mouse.press",       RH_TIER_UPDATE, rh_verb_input_mouse_press       },
    { "input.mouse.release",     RH_TIER_UPDATE, rh_verb_input_mouse_release     },
    { "input.mouse.drag",        RH_TIER_UPDATE, rh_verb_input_mouse_drag        },
    { "input.keyboard.key_down", RH_TIER_UPDATE, rh_verb_input_keyboard_key_down },
    { "input.keyboard.key_up",   RH_TIER_UPDATE, rh_verb_input_keyboard_key_up   },

    /* file.* */
    { "file.read",           RH_TIER_READ,        rh_verb_file_read           },
    { "file.write",          RH_TIER_UPDATE,      rh_verb_file_write          },
    { "file.write_at",       RH_TIER_UPDATE,      rh_verb_file_write_at       },
    { "file.stat",           RH_TIER_READ,        rh_verb_file_stat           },
    { "file.delete",         RH_TIER_DELETE,      rh_verb_file_delete         },
    { "file.exists",         RH_TIER_READ,        rh_verb_file_exists         },
    { "file.wait",           RH_TIER_READ,        rh_verb_file_wait           },
    { "file.rename",         RH_TIER_UPDATE,      rh_verb_file_rename         },
    { "file.create",         RH_TIER_CREATE,      rh_verb_file_create         },
    { "file.download",       RH_TIER_CREATE,      rh_verb_file_download       },

    /* directory.* */
    { "directory.list",      RH_TIER_READ,        rh_verb_directory_list      },
    { "directory.stat",      RH_TIER_READ,        rh_verb_directory_stat      },
    { "directory.exists",    RH_TIER_READ,        rh_verb_directory_exists    },
    { "directory.create",    RH_TIER_CREATE,      rh_verb_directory_create    },
    { "directory.rename",    RH_TIER_UPDATE,      rh_verb_directory_rename    },
    { "directory.remove",    RH_TIER_DELETE,      rh_verb_directory_remove    },

    /* process.* */
    { "process.list",        RH_TIER_READ,        rh_verb_process_list        },
    { "process.start",       RH_TIER_CREATE,      rh_verb_process_start       },
    { "process.shell",       RH_TIER_CREATE,      rh_verb_process_shell       },
    { "process.kill",        RH_TIER_DELETE,      rh_verb_process_kill        },
    { "process.wait",        RH_TIER_READ,        rh_verb_process_wait        },

    /* registry.* (v2.0 names kept as back-compat aliases) */
    { "registry.read",          RH_TIER_READ,    rh_verb_registry_read         },
    { "registry.write",         RH_TIER_UPDATE,  rh_verb_registry_write        },
    { "registry.delete",        RH_TIER_DELETE,  rh_verb_registry_delete       },
    { "registry.wait",          RH_TIER_READ,    rh_verb_registry_wait         },

    /* registry.* v2.1 namespace split */
    { "registry.key.read",      RH_TIER_READ,    rh_verb_registry_key_read     },
    { "registry.key.delete",    RH_TIER_DELETE,  rh_verb_registry_key_delete   },
    { "registry.value.read",    RH_TIER_READ,    rh_verb_registry_value_read   },
    { "registry.value.create",  RH_TIER_CREATE,  rh_verb_registry_value_create },
    { "registry.value.update",  RH_TIER_UPDATE,  rh_verb_registry_value_update },
    { "registry.value.delete",  RH_TIER_DELETE,  rh_verb_registry_value_delete },

    /* clipboard.* */
    { "clipboard.get",       RH_TIER_READ,        rh_verb_clipboard_get       },
    { "clipboard.set",       RH_TIER_UPDATE,      rh_verb_clipboard_set       },

    /* watch.* (registration only; EVENT delivery is out of scope for the
     * single-threaded classic model and the conformance suite does not
     * exercise delivery -- it only checks sub-id allocation + idempotent
     * cancel). watch.element is omitted per D9. */
    { "watch.window",        RH_TIER_READ,        rh_verb_watch_window        },
    { "watch.process",       RH_TIER_READ,        rh_verb_watch_process       },
    { "watch.region",        RH_TIER_READ,        rh_verb_watch_region        },
    { "watch.file",          RH_TIER_READ,        rh_verb_watch_file          },
    { "watch.registry",      RH_TIER_READ,        rh_verb_watch_registry      },
    { "watch.cancel",        RH_TIER_READ,        rh_verb_watch_cancel        }
};

static const VerbEntry* find_verb(const char* verb)
{
    int i;
    for (i = 0; i < (int)(sizeof(kVerbs) / sizeof(kVerbs[0])); ++i) {
        if (strcmp(kVerbs[i].verb, verb) == 0) {
            return &kVerbs[i];
        }
    }
    return NULL;
}

int rh_verb_table_count(void)
{
    return (int)(sizeof(kVerbs) / sizeof(kVerbs[0]));
}

void rh_verb_table_get(int i, const char** verb_out,
                       const char** tier_name_out)
{
    if (i < 0 || i >= rh_verb_table_count()) {
        if (verb_out != NULL)      *verb_out = NULL;
        if (tier_name_out != NULL) *tier_name_out = NULL;
        return;
    }
    if (verb_out != NULL) {
        *verb_out = kVerbs[i].verb;
    }
    if (tier_name_out != NULL) {
        *tier_name_out = rh_tier_name(kVerbs[i].tier);
    }
}

/* --- connection.* handlers (port of shared/connection.cpp) ------------- */

static void handle_hello(RhConn* c, const RhRequest* req)
{
    const char* version;

    if (req->argc != 2) {
        err_kv(c->sock, "invalid_args", "message",
               "connection.hello requires <client-name> <protocol-version>");
        return;
    }
    version = req->args[1];
    if (!version_classic_ok(version)) {
        err_kv2(c->sock, "protocol_mismatch", "agent", "2",
                "client", version);
        return;
    }
    c->state = RH_ST_CONNECTED;
    rh_send_ok(c->sock);
}

static void handle_tier_raise(RhConn* c, const RhRequest* req)
{
    int requested;

    if (req->argc != 2) {
        err_kv(c->sock, "invalid_args", "message",
               "connection.tier_raise requires <tier> <token>");
        return;
    }
    requested = rh_tier_from_name(req->args[0]);
    if (requested < 0) {
        err_kv(c->sock, "invalid_args", "message", "unknown tier");
        return;
    }
    if (requested <= c->tier) {
        err_kv2(c->sock, "invalid_args",
                "message", "use tier_drop for downgrades",
                "current", rh_tier_name(c->tier));
        return;
    }
    if (!rh_token_verify(req->args[1])) {
        err_kv(c->sock, "auth_invalid", "message", "token mismatch");
        return;
    }
    c->tier = requested;
    ok_kv(c->sock, "new_tier", rh_tier_name(c->tier));
}

static void handle_tier_drop(RhConn* c, const RhRequest* req)
{
    int requested;

    if (req->argc != 1) {
        err_kv(c->sock, "invalid_args", "message",
               "connection.tier_drop requires <tier>");
        return;
    }
    requested = rh_tier_from_name(req->args[0]);
    if (requested < 0) {
        err_kv(c->sock, "invalid_args", "message", "unknown tier");
        return;
    }
    if (requested > c->tier) {
        err_kv2(c->sock, "invalid_args",
                "message", "use tier_raise for upgrades",
                "current", rh_tier_name(c->tier));
        return;
    }
    c->tier = requested;
    ok_kv(c->sock, "new_tier", rh_tier_name(c->tier));
}

static void handle_close(RhConn* c)
{
    /* W8 has no subscriptions to drain (EVENT framing is W9). */
    rh_send_ok(c->sock);
    c->state = RH_ST_CLOSED;
}

/* --- dispatch ---------------------------------------------------------- */

static void dispatch(RhConn* c, const RhRequest* req)
{
    const VerbEntry* entry;

#ifdef RH_DEBUG
    {
        char _dbg[512];
        int  _pos;
        int  _di;
        int  _n;
        _snprintf(_dbg, sizeof(_dbg), ">> %s", req->verb);
        _pos = (int)strlen(_dbg);
        for (_di = 0; _di < req->argc && _pos < (int)sizeof(_dbg) - 1; ++_di) {
            _n = _snprintf(_dbg + _pos, (int)sizeof(_dbg) - _pos,
                           " %s", req->args[_di]);
            if (_n > 0) _pos += _n;
        }
        rh_dbg("%s", _dbg);
    }
#endif

    /* Header framed cleanly but args were malformed (PROTOCOL.md 1.2.5). */
    if (req->parse_error) {
        err_kv(c->sock, "invalid_args", "message",
               req->parse_message ? req->parse_message
                                   : "malformed header");
        return;
    }

    /* Pre-hello restricts the verb surface (PROTOCOL.md 2.1). */
    if (c->state == RH_ST_PRE_HELLO) {
        if (strcmp(req->verb, "connection.hello") == 0) {
            handle_hello(c, req);
            return;
        }
        if (strcmp(req->verb, "connection.close") == 0) {
            handle_close(c);
            return;
        }
        err_kv(c->sock, "invalid_state", "required", "hello");
        return;
    }

    /* Connected. connection.* verbs are tier-agnostic. */
    if (strcmp(req->verb, "connection.hello") == 0) {
        err_kv(c->sock, "invalid_state", "message", "already hello'd");
        return;
    }
    if (strcmp(req->verb, "connection.tier_raise") == 0) {
        handle_tier_raise(c, req);
        return;
    }
    if (strcmp(req->verb, "connection.tier_drop") == 0) {
        handle_tier_drop(c, req);
        return;
    }
    if (strcmp(req->verb, "connection.reset") == 0) {
        rh_reader_flush(&c->reader);
        rh_send_ok(c->sock);
        return;
    }
    if (strcmp(req->verb, "connection.close") == 0) {
        handle_close(c);
        return;
    }

    /* Verb table (system.*). */
    entry = find_verb(req->verb);
    if (entry != NULL) {
        if (c->tier < entry->tier) {
            err_kv2(c->sock, "tier_required",
                    "required", rh_tier_name(entry->tier),
                    "current",  rh_tier_name(c->tier));
            return;
        }
        entry->fn(c, req);
        return;
    }

    /* Not implemented in this build (PROTOCOL.md 5.1 -> not_supported,
     * NOT "unknown_verb" as the W8 prompt sketched). */
    err_kv2(c->sock, "not_supported",
            "verb", req->verb,
            "reason", "not implemented in this build");
}

/* --- connection lifecycle --------------------------------------------- */

void rh_connection_run(SOCKET s)
{
    RhConn    c;
    RhRequest req;
    int       rc;

    c.sock  = s;
    c.state = RH_ST_PRE_HELLO;
    c.tier  = RH_TIER_READ;

    if (rh_reader_init(&c.reader, s) != RH_PROTO_OK) {
        closesocket(s);
        return;
    }

    while (c.state != RH_ST_CLOSED) {
        rc = rh_read_request(&c.reader, &req);
        if (rc != RH_PROTO_OK) {
            break;   /* EOF or fatal wire error */
        }
        dispatch(&c, &req);
    }

    rh_reader_free(&c.reader);
    closesocket(s);
}

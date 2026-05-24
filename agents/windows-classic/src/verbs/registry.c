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

#include "registry.h"
#include "common.h"
#include "../json.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

/* REG_QWORD arrived in the Win2000-era SDK; guard for a barebones VC98
 * winnt.h if the Platform SDK include path is absent. */
#ifndef REG_QWORD
#define REG_QWORD 11
#endif
#ifndef REG_DWORD_BIG_ENDIAN
#define REG_DWORD_BIG_ENDIAN 5
#endif
#ifndef REG_LINK
#define REG_LINK 6
#endif

/* v2.1 single value-data cap. Win32 hard limit is ~1 MB but the wire-arg
 * cap (RH_MAX_ARG_LEN = 512) gates the practical incoming size for create/
 * update; reads cap data at REG_DATA_READ_CAP to stay within the JSON
 * envelope. */
#define REG_DATA_READ_CAP   16384

/* Split "HKLM\Sub\Key" into a root HKEY + subkey pointer. Returns 1 on a
 * recognised root, 0 otherwise (-> ERR invalid_args). `subkey` points into
 * the original string (may be ""). */
static int split_root(const char* path, HKEY* root, const char** subkey)
{
    const char* bs;
    int         n;
    char        head[32];

    bs = strchr(path, '\\');
    if (bs != NULL) {
        n = (int)(bs - path);
    } else {
        n = (int)strlen(path);
    }
    if (n <= 0 || n >= (int)sizeof(head)) {
        return 0;
    }
    memcpy(head, path, (size_t)n);
    head[n] = '\0';

    if (strcmp(head, "HKLM") == 0 ||
        strcmp(head, "HKEY_LOCAL_MACHINE") == 0) {
        *root = HKEY_LOCAL_MACHINE;
    } else if (strcmp(head, "HKCU") == 0 ||
               strcmp(head, "HKEY_CURRENT_USER") == 0) {
        *root = HKEY_CURRENT_USER;
    } else if (strcmp(head, "HKCR") == 0 ||
               strcmp(head, "HKEY_CLASSES_ROOT") == 0) {
        *root = HKEY_CLASSES_ROOT;
    } else if (strcmp(head, "HKU") == 0 ||
               strcmp(head, "HKEY_USERS") == 0) {
        *root = HKEY_USERS;
    } else if (strcmp(head, "HKCC") == 0 ||
               strcmp(head, "HKEY_CURRENT_CONFIG") == 0) {
        *root = HKEY_CURRENT_CONFIG;
    } else {
        return 0;
    }
    *subkey = (bs != NULL) ? (bs + 1) : "";
    return 1;
}

static const char* reg_type_name(DWORD t)
{
    switch (t) {
        case REG_SZ:                 return "REG_SZ";
        case REG_EXPAND_SZ:          return "REG_EXPAND_SZ";
        case REG_DWORD:              return "REG_DWORD";
        case REG_DWORD_BIG_ENDIAN:   return "REG_DWORD_BIG_ENDIAN";
        case REG_BINARY:             return "REG_BINARY";
        case REG_MULTI_SZ:           return "REG_MULTI_SZ";
        case REG_QWORD:              return "REG_QWORD";
        case REG_LINK:               return "REG_LINK";
        default:                     return "REG_NONE";
    }
}

/* Spec REG_* type-name -> Win32 constant. Returns 1 if recognised, 0
 * otherwise (-> ERR invalid_args). */
static int reg_type_from_name(const char* s, DWORD* out)
{
    if (strcmp(s, "REG_SZ") == 0)                 { *out = REG_SZ;                 return 1; }
    if (strcmp(s, "REG_EXPAND_SZ") == 0)          { *out = REG_EXPAND_SZ;          return 1; }
    if (strcmp(s, "REG_DWORD") == 0)              { *out = REG_DWORD;              return 1; }
    if (strcmp(s, "REG_DWORD_BIG_ENDIAN") == 0)   { *out = REG_DWORD_BIG_ENDIAN;   return 1; }
    if (strcmp(s, "REG_BINARY") == 0)             { *out = REG_BINARY;             return 1; }
    if (strcmp(s, "REG_MULTI_SZ") == 0)           { *out = REG_MULTI_SZ;           return 1; }
    if (strcmp(s, "REG_QWORD") == 0)              { *out = REG_QWORD;              return 1; }
    if (strcmp(s, "REG_LINK") == 0)               { *out = REG_LINK;               return 1; }
    if (strcmp(s, "REG_NONE") == 0)               { *out = REG_NONE;               return 1; }
    return 0;
}

/* --- shared helpers (used by both v2.0 and v2.1 handlers) -------------- */

/* Open an existing key under hive+subpath. Returns ERROR_SUCCESS on hit,
 * else a Win32 LONG (use rh_win32_code()). RegOpenKeyExA does NOT create. */
static LONG rh_classic_reg_open(const char* path_with_hive, REGSAM access,
                                HKEY* out)
{
    HKEY        root;
    const char* sub;

    if (!split_root(path_with_hive, &root, &sub)) {
        return ERROR_INVALID_PARAMETER;
    }
    return RegOpenKeyExA(root, sub, 0, access, out);
}

/* Map an empty value name ("") to NULL so RegQueryValueExA / RegSetValueExA
 * address the (Default) value. */
static const char* default_value_translate(const char* name)
{
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    return name;
}

/* --- base64 (RFC 4648, no whitespace, '=' pad) ------------------------- */

static const char k_b64_enc[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Encode `len` bytes from `src` as base64 into `dst`. Caller sizes
 * `dst_cap` >= ((len + 2) / 3) * 4 + 1. Returns the written length
 * (excluding NUL). */
static int b64_encode(const unsigned char* src, int len,
                      char* dst, int dst_cap)
{
    int i;
    int o;
    int need;

    need = ((len + 2) / 3) * 4 + 1;
    if (need > dst_cap) {
        return -1;
    }
    o = 0;
    i = 0;
    while (i + 3 <= len) {
        unsigned a = src[i];
        unsigned b = src[i + 1];
        unsigned c = src[i + 2];
        dst[o++] = k_b64_enc[(a >> 2) & 0x3F];
        dst[o++] = k_b64_enc[((a << 4) | (b >> 4)) & 0x3F];
        dst[o++] = k_b64_enc[((b << 2) | (c >> 6)) & 0x3F];
        dst[o++] = k_b64_enc[c & 0x3F];
        i += 3;
    }
    if (i < len) {
        unsigned a = src[i];
        unsigned b = (i + 1 < len) ? src[i + 1] : 0;
        dst[o++] = k_b64_enc[(a >> 2) & 0x3F];
        dst[o++] = k_b64_enc[((a << 4) | (b >> 4)) & 0x3F];
        if (i + 1 < len) {
            dst[o++] = k_b64_enc[(b << 2) & 0x3F];
        } else {
            dst[o++] = '=';
        }
        dst[o++] = '=';
    }
    dst[o] = '\0';
    return o;
}

/* Map a base64 character to its 6-bit value, or 255 for invalid / pad. */
static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return 26 + (c - 'a');
    if (c >= '0' && c <= '9') return 52 + (c - '0');
    if (c == '+')             return 62;
    if (c == '/')             return 63;
    return 255;
}

/* Decode base64 `src` (NUL-terminated, may contain trailing '=') into
 * `dst`. Returns the written byte count, or -1 on overflow / invalid. */
static int b64_decode(const char* src, unsigned char* dst, int dst_cap)
{
    int slen;
    int i;
    int o;
    unsigned acc;
    int      bits;

    slen = (int)strlen(src);
    /* Strip '=' padding for processing; track effective length. */
    while (slen > 0 && src[slen - 1] == '=') {
        --slen;
    }
    acc  = 0;
    bits = 0;
    o    = 0;
    for (i = 0; i < slen; ++i) {
        int v = b64_val(src[i]);
        if (v == 255) {
            return -1;
        }
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= dst_cap) {
                return -1;
            }
            dst[o++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    return o;
}

/* --- value-data marshal (string in / Win32 buffer out) ----------------- */

/* Translate the two-char escape `\n` (backslash + 'n') to a literal '\n'
 * in-place. Used for REG_MULTI_SZ entry separators on the wire (the
 * tokenizer rejects literal whitespace inside a positional). Returns the
 * new length. */
static int unescape_backslash_n(char* s)
{
    int r = 0;
    int w = 0;
    while (s[r] != '\0') {
        if (s[r] == '\\' && s[r + 1] == 'n') {
            s[w++] = '\n';
            r += 2;
        } else {
            s[w++] = s[r++];
        }
    }
    s[w] = '\0';
    return w;
}

/* Build a Win32 value buffer for the given REG_* type from the wire data
 * string. Returns 1 and populates *out_buf (malloc'd) + *out_len + *out_type
 * on success; returns 0 on parse/type failure (caller emits invalid_args).
 *
 * Win32 wants:
 *   REG_SZ / REG_EXPAND_SZ / REG_LINK: bytes with trailing NUL
 *   REG_DWORD                        : 4 little-endian bytes
 *   REG_DWORD_BIG_ENDIAN             : 4 big-endian bytes
 *   REG_QWORD                        : 8 little-endian bytes
 *   REG_BINARY                       : raw bytes
 *   REG_MULTI_SZ                     : NUL-separated, double-NUL terminated
 *   REG_NONE                         : raw bytes (no shape)
 */
static int build_value_data(DWORD type, const char* data,
                            BYTE** out_buf, DWORD* out_len)
{
    if (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_LINK) {
        size_t n = strlen(data) + 1;
        BYTE*  b = (BYTE*)malloc(n);
        if (b == NULL) return 0;
        memcpy(b, data, n);
        *out_buf = b;
        *out_len = (DWORD)n;
        return 1;
    }
    if (type == REG_DWORD || type == REG_DWORD_BIG_ENDIAN) {
        unsigned long v;
        char*         end = NULL;
        BYTE*         b;
        v = strtoul(data, &end, 0);
        if (end == data || (end != NULL && *end != '\0')) {
            return 0;
        }
        b = (BYTE*)malloc(4);
        if (b == NULL) return 0;
        if (type == REG_DWORD) {
            b[0] = (BYTE)(v & 0xFF);
            b[1] = (BYTE)((v >> 8) & 0xFF);
            b[2] = (BYTE)((v >> 16) & 0xFF);
            b[3] = (BYTE)((v >> 24) & 0xFF);
        } else {
            b[0] = (BYTE)((v >> 24) & 0xFF);
            b[1] = (BYTE)((v >> 16) & 0xFF);
            b[2] = (BYTE)((v >> 8) & 0xFF);
            b[3] = (BYTE)(v & 0xFF);
        }
        *out_buf = b;
        *out_len = 4;
        return 1;
    }
    if (type == REG_QWORD) {
        unsigned __int64 v;
        char*            end = NULL;
        BYTE*            b;
        int              i;
        v = _strtoui64(data, &end, 0);
        if (end == data || (end != NULL && *end != '\0')) {
            return 0;
        }
        b = (BYTE*)malloc(8);
        if (b == NULL) return 0;
        for (i = 0; i < 8; ++i) {
            b[i] = (BYTE)((v >> (i * 8)) & 0xFF);
        }
        *out_buf = b;
        *out_len = 8;
        return 1;
    }
    if (type == REG_BINARY || type == REG_NONE) {
        int    cap = (int)strlen(data);   /* base64 out <= input */
        BYTE*  b;
        int    n;
        if (cap < 1) cap = 1;
        b = (BYTE*)malloc((size_t)cap);
        if (b == NULL) return 0;
        n = b64_decode(data, b, cap);
        if (n < 0) {
            free(b);
            return 0;
        }
        *out_buf = b;
        *out_len = (DWORD)n;
        return 1;
    }
    if (type == REG_MULTI_SZ) {
        char*  tmp;
        int    n;
        int    i;
        BYTE*  b;
        size_t total;
        size_t in_len = strlen(data) + 1;
        tmp = (char*)malloc(in_len);
        if (tmp == NULL) return 0;
        memcpy(tmp, data, in_len);
        n = unescape_backslash_n(tmp);
        /* Convert \n separators to NULs; append double-NUL. Empty input
         * still emits the lone double-NUL "\0\0". */
        for (i = 0; i < n; ++i) {
            if (tmp[i] == '\n') tmp[i] = '\0';
        }
        total = (size_t)n + 2;
        b = (BYTE*)malloc(total);
        if (b == NULL) {
            free(tmp);
            return 0;
        }
        if (n > 0) memcpy(b, tmp, (size_t)n);
        b[n]     = '\0';
        b[n + 1] = '\0';
        free(tmp);
        *out_buf = b;
        *out_len = (DWORD)total;
        return 1;
    }
    return 0;
}

/* Emit a JSON-quoted string from a length-bounded buffer, escaping JSON
 * specials including NUL (as \u0000). Needed for REG_MULTI_SZ, whose buffer
 * is a sequence of NUL-separated C strings; rh_json_str() terminates at the
 * first '\0' and would silently truncate. Writes the quoted result --
 * including the surrounding double quotes -- via rh_json_raw().
 *
 * dst_cap should be at least 6 * len + 3 to cover the worst case (every
 * byte becomes "\u00XX" plus the two quotes plus NUL). */
static void emit_quoted_blob(RhJson* j, const BYTE* src, DWORD len,
                             char* dst, int dst_cap)
{
    static const char kHex[] = "0123456789abcdef";
    int       o = 0;
    DWORD     i;
    unsigned  c;

    if (dst_cap < 3) {
        rh_json_str(j, "");
        return;
    }
    dst[o++] = '"';
    for (i = 0; i < len; ++i) {
        c = (unsigned)src[i];
        if (o + 6 + 1 > dst_cap) {        /* +1 for closing quote */
            /* Overflow: bail with empty string rather than truncate. */
            rh_json_str(j, "");
            return;
        }
        switch (c) {
            case '"':  dst[o++] = '\\'; dst[o++] = '"';  break;
            case '\\': dst[o++] = '\\'; dst[o++] = '\\'; break;
            case '\b': dst[o++] = '\\'; dst[o++] = 'b';  break;
            case '\f': dst[o++] = '\\'; dst[o++] = 'f';  break;
            case '\n': dst[o++] = '\\'; dst[o++] = 'n';  break;
            case '\r': dst[o++] = '\\'; dst[o++] = 'r';  break;
            case '\t': dst[o++] = '\\'; dst[o++] = 't';  break;
            default:
                if (c < 0x20) {
                    dst[o++] = '\\'; dst[o++] = 'u';
                    dst[o++] = '0';  dst[o++] = '0';
                    dst[o++] = kHex[(c >> 4) & 0x0F];
                    dst[o++] = kHex[c & 0x0F];
                } else {
                    dst[o++] = (char)c;
                }
                break;
        }
    }
    dst[o++] = '"';
    dst[o]   = '\0';
    rh_json_raw(j, dst);
}

/* Emit the {"type": "...", "data": "..."} body for the v2.1
 * registry.value.read response. */
static void emit_value_body(RhJson* j, DWORD type,
                            const BYTE* data, DWORD len)
{
    rh_json_begin_obj(j);
    rh_json_key(j, "type");
    rh_json_str(j, reg_type_name(type));
    rh_json_key(j, "data");

    if ((type == REG_SZ || type == REG_EXPAND_SZ || type == REG_LINK)) {
        /* Win32 stores these as char-terminated; trim trailing NUL if
         * present so the JSON string isn't padded. */
        if (len > 0 && data[len - 1] == '\0') {
            /* in-place safe: emitter just reads the C string */
            rh_json_str(j, (const char*)data);
        } else {
            /* defensive: copy + NUL-terminate */
            char* tmp = (char*)malloc((size_t)len + 1);
            if (tmp == NULL) {
                rh_json_str(j, "");
            } else {
                memcpy(tmp, data, (size_t)len);
                tmp[len] = '\0';
                rh_json_str(j, tmp);
                free(tmp);
            }
        }
    } else if (type == REG_DWORD && len == 4) {
        char buf[16];
        unsigned long v = (unsigned long)data[0]
                        | ((unsigned long)data[1] << 8)
                        | ((unsigned long)data[2] << 16)
                        | ((unsigned long)data[3] << 24);
        _snprintf(buf, sizeof(buf), "%lu", v);
        buf[sizeof(buf) - 1] = '\0';
        rh_json_str(j, buf);
    } else if (type == REG_DWORD_BIG_ENDIAN && len == 4) {
        char buf[16];
        unsigned long v = (unsigned long)data[3]
                        | ((unsigned long)data[2] << 8)
                        | ((unsigned long)data[1] << 16)
                        | ((unsigned long)data[0] << 24);
        _snprintf(buf, sizeof(buf), "%lu", v);
        buf[sizeof(buf) - 1] = '\0';
        rh_json_str(j, buf);
    } else if (type == REG_QWORD && len == 8) {
        char             buf[32];
        unsigned __int64 v = 0;
        int              i;
        for (i = 7; i >= 0; --i) {
            v = (v << 8) | (unsigned __int64)data[i];
        }
        _snprintf(buf, sizeof(buf), "%I64u", v);
        buf[sizeof(buf) - 1] = '\0';
        rh_json_str(j, buf);
    } else if (type == REG_MULTI_SZ) {
        /* NUL-separated strings with a trailing double-NUL. The data
         * field carries the raw buffer (embedded NULs included); those
         * NULs surface as \u0000 escapes in the JSON output. The default
         * rh_json_str path would terminate at the first NUL and silently
         * truncate, so route the length-bounded buffer through
         * emit_quoted_blob() (which emits the surrounding quotes itself
         * via rh_json_raw). */
        int   cap;
        char* tmp;
        cap = (int)len * 6 + 3;
        if (cap < 3) cap = 3;
        tmp = (char*)malloc((size_t)cap);
        if (tmp == NULL) {
            rh_json_str(j, "");
        } else {
            emit_quoted_blob(j, data, (DWORD)len, tmp, cap);
            free(tmp);
        }
    } else if (type == REG_BINARY || type == REG_NONE) {
        int   cap = ((int)len + 2) / 3 * 4 + 1;
        char* tmp = (char*)malloc((size_t)cap);
        if (tmp == NULL) {
            rh_json_str(j, "");
        } else {
            int n = b64_encode(data, (int)len, tmp, cap);
            if (n < 0) {
                rh_json_str(j, "");
            } else {
                rh_json_str(j, tmp);
            }
            free(tmp);
        }
    } else {
        rh_json_str(j, "");
    }
    rh_json_end_obj(j);
}

/* --- registry.read (v2.0 — back-compat) -------------------------------- */
/*
 * Kept as a thin retained handler for clients on the pre-v2.1 namespace.
 * Mixed-mode: with --value it returns {type,data}; without, it returns
 * {values{...}, subkeys[...]}. The v2.1 split (registry.value.read and
 * registry.key.read) is preferred -- see Planning/full-spec-completion/
 * per-verb/registry.namespace-classic.md.
 */
void rh_verb_registry_read(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = { { "--value", 1 } };
    RhArgs      a;
    HKEY        root;
    const char* subkey;
    HKEY        key;
    LONG        rc;
    RhJson*     j;
    const char* r;

    rh_args_parse(req, defs, 1, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "registry.read requires <path>");
        return;
    }
    if (!split_root(a.pos[0], &root, &subkey)) {
        rh_err_msg(c, "invalid_args", "unknown registry root");
        return;
    }
    rc = RegOpenKeyExA(root, subkey, 0, KEY_READ, &key);
    if (rc != ERROR_SUCCESS) {
        rh_err(c, "not_found");
        return;
    }

    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        RegCloseKey(key);
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);

    if (a.val[0] != NULL) {
        /* Single value. */
        DWORD type;
        BYTE  data[4096];
        DWORD dlen = (DWORD)sizeof(data);
        rc = RegQueryValueExA(key, a.val[0], NULL, &type, data, &dlen);
        if (rc != ERROR_SUCCESS) {
            free(j);
            RegCloseKey(key);
            rh_err(c, "not_found");
            return;
        }
        rh_json_begin_obj(j);
        rh_json_key(j, "type");
        rh_json_str(j, reg_type_name(type));
        rh_json_key(j, "data");
        if (type == REG_DWORD && dlen == 4) {
            char num[16];
            _snprintf(num, sizeof(num), "%lu",
                      (unsigned long)(*(DWORD*)data));
            num[sizeof(num) - 1] = '\0';
            rh_json_str(j, num);
        } else if (type == REG_SZ || type == REG_EXPAND_SZ) {
            data[sizeof(data) - 1] = 0;
            rh_json_str(j, (const char*)data);
        } else {
            rh_json_str(j, "");
        }
        rh_json_end_obj(j);
    } else {
        /* Whole key: values + subkeys. */
        DWORD i;
        char  name[512];
        DWORD nlen;
        DWORD type;
        BYTE  data[2048];
        DWORD dlen;

        rh_json_begin_obj(j);
        rh_json_key(j, "values");
        rh_json_begin_obj(j);
        i = 0;
        for (;;) {
            nlen = (DWORD)sizeof(name);
            dlen = (DWORD)sizeof(data);
            rc = RegEnumValueA(key, i, name, &nlen, NULL,
                               &type, data, &dlen);
            if (rc != ERROR_SUCCESS) {
                break;
            }
            rh_json_key(j, name);
            if (type == REG_DWORD && dlen == 4) {
                rh_json_int(j, (i32)(*(DWORD*)data));
            } else if ((type == REG_SZ || type == REG_EXPAND_SZ)) {
                data[sizeof(data) - 1] = 0;
                rh_json_str(j, (const char*)data);
            } else {
                rh_json_str(j, "");
            }
            ++i;
        }
        rh_json_end_obj(j);

        rh_json_key(j, "subkeys");
        rh_json_begin_arr(j);
        i = 0;
        for (;;) {
            nlen = (DWORD)sizeof(name);
            rc = RegEnumKeyExA(key, i, name, &nlen,
                               NULL, NULL, NULL, NULL);
            if (rc != ERROR_SUCCESS) {
                break;
            }
            rh_json_arr_str(j, name);
            ++i;
        }
        rh_json_end_arr(j);
        rh_json_end_obj(j);
    }

    RegCloseKey(key);
    r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
}

/* --- registry.write (v2.0 — back-compat) ------------------------------- */

void rh_verb_registry_write(RhConn* c, const RhRequest* req)
{
    RhArgs      a;
    HKEY        root;
    const char* subkey;
    HKEY        key;
    LONG        rc;
    const char* tname;
    const char* data;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 4) {
        rh_err_msg(c, "invalid_args",
                   "registry.write requires <path> <name> <type> <data>");
        return;
    }
    if (!split_root(a.pos[0], &root, &subkey)) {
        rh_err_msg(c, "invalid_args", "unknown registry root");
        return;
    }
    rc = RegCreateKeyExA(root, subkey, 0, NULL, 0,
                         KEY_SET_VALUE, NULL, &key, NULL);
    if (rc != ERROR_SUCCESS) {
        rh_err(c, rh_win32_code((DWORD)rc));
        return;
    }
    tname = a.pos[2];
    data  = a.pos[3];
    if (strcmp(tname, "REG_DWORD") == 0) {
        DWORD v = (DWORD)strtoul(data, NULL, 0);
        rc = RegSetValueExA(key, a.pos[1], 0, REG_DWORD,
                            (const BYTE*)&v, sizeof(v));
    } else {
        /* REG_SZ / REG_EXPAND_SZ / default: store as string. */
        DWORD t = (strcmp(tname, "REG_EXPAND_SZ") == 0)
                  ? REG_EXPAND_SZ : REG_SZ;
        rc = RegSetValueExA(key, a.pos[1], 0, t,
                            (const BYTE*)data,
                            (DWORD)(strlen(data) + 1));
    }
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        rh_err(c, rh_win32_code((DWORD)rc));
        return;
    }
    rh_ok(c);
}

/* --- registry.delete (v2.0 — back-compat) ------------------------------ */

void rh_verb_registry_delete(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = { { "--value", 1 } };
    RhArgs      a;
    HKEY        root;
    const char* subkey;
    LONG        rc;

    rh_args_parse(req, defs, 1, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "registry.delete requires <path>");
        return;
    }
    if (!split_root(a.pos[0], &root, &subkey)) {
        rh_err_msg(c, "invalid_args", "unknown registry root");
        return;
    }
    if (a.val[0] != NULL) {
        HKEY key;
        rc = RegOpenKeyExA(root, subkey, 0, KEY_SET_VALUE, &key);
        if (rc != ERROR_SUCCESS) {
            rh_err(c, "not_found");
            return;
        }
        rc = RegDeleteValueA(key, a.val[0]);
        RegCloseKey(key);
    } else {
        rc = RegDeleteKeyA(root, subkey);
    }
    if (rc != ERROR_SUCCESS) {
        rh_err(c, (rc == ERROR_FILE_NOT_FOUND) ? "not_found"
                                               : rh_win32_code((DWORD)rc));
        return;
    }
    rh_ok(c);
}

/* --- registry.wait ----------------------------------------------------- */

void rh_verb_registry_wait(RhConn* c, const RhRequest* req)
{
    RhArgs      a;
    HKEY        root;
    const char* subkey;
    HKEY        key;
    LONG        rc;
    HANDLE      ev;
    int         timeout_ms;
    DWORD       wr;
    RhJson*     j;
    const char* r;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 2) {
        rh_err_msg(c, "invalid_args",
                   "registry.wait requires <path> <timeout-ms>");
        return;
    }
    if (!split_root(a.pos[0], &root, &subkey)) {
        rh_err_msg(c, "invalid_args", "unknown registry root");
        return;
    }
    timeout_ms = atoi(a.pos[1]);
    rc = RegOpenKeyExA(root, subkey, 0, KEY_NOTIFY, &key);
    if (rc != ERROR_SUCCESS) {
        rh_err(c, "not_found");
        return;
    }
    ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (ev == NULL) {
        RegCloseKey(key);
        rh_err(c, "wire_desync");
        return;
    }
    rc = RegNotifyChangeKeyValue(key, TRUE,
                                 REG_NOTIFY_CHANGE_NAME |
                                 REG_NOTIFY_CHANGE_LAST_SET,
                                 ev, TRUE);
    if (rc != ERROR_SUCCESS) {
        CloseHandle(ev);
        RegCloseKey(key);
        rh_err_msg(c, "not_supported", "registry change notify unavailable");
        return;
    }
    wr = WaitForSingleObject(ev, (DWORD)timeout_ms);
    CloseHandle(ev);
    RegCloseKey(key);
    if (wr == WAIT_TIMEOUT) {
        rh_err_kv(c, "timeout", "deadline", a.pos[1]);
        return;
    }
    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);
    rh_json_begin_obj(j);
    rh_json_key(j, "path");
    rh_json_str(j, a.pos[0]);
    rh_json_end_obj(j);
    r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
}

/* ====================================================================== */
/* === v2.1 namespace split: registry.key.* / registry.value.* =========== */
/* ====================================================================== */

/* --- registry.key.read ------------------------------------------------- */
/*
 * Input  : <path>
 * Output : { "subkeys": [string], "values": [{"name","type"}] }
 *
 * Enumerates immediate children of `path` -- one level only. Per the spec
 * (registry.key.read.json) the `values` array carries names + types only;
 * data is fetched separately via registry.value.read.
 */
void rh_verb_registry_key_read(RhConn* c, const RhRequest* req)
{
    RhArgs      a;
    HKEY        key;
    LONG        rc;
    RhJson*     j;
    const char* r;
    DWORD       i;
    char        name[512];
    DWORD       nlen;
    DWORD       type;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "registry.key.read requires <path>");
        return;
    }
    rc = rh_classic_reg_open(a.pos[0], KEY_READ, &key);
    if (rc == ERROR_INVALID_PARAMETER) {
        rh_err_msg(c, "invalid_args", "unknown registry root");
        return;
    }
    if (rc != ERROR_SUCCESS) {
        rh_err(c, (rc == ERROR_FILE_NOT_FOUND ||
                   rc == ERROR_PATH_NOT_FOUND)
                  ? "not_found" : rh_win32_code((DWORD)rc));
        return;
    }

    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        RegCloseKey(key);
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);
    rh_json_begin_obj(j);

    /* subkeys: [string, ...] */
    rh_json_key(j, "subkeys");
    rh_json_begin_arr(j);
    i = 0;
    for (;;) {
        nlen = (DWORD)sizeof(name);
        rc = RegEnumKeyExA(key, i, name, &nlen,
                           NULL, NULL, NULL, NULL);
        if (rc != ERROR_SUCCESS) {
            break;
        }
        rh_json_arr_str(j, name);
        ++i;
    }
    rh_json_end_arr(j);

    /* values: [{"name":..., "type":...}, ...] */
    rh_json_key(j, "values");
    rh_json_begin_arr(j);
    i = 0;
    for (;;) {
        nlen = (DWORD)sizeof(name);
        rc = RegEnumValueA(key, i, name, &nlen, NULL,
                           &type, NULL, NULL);
        if (rc != ERROR_SUCCESS) {
            break;
        }
        rh_json_arr_elem(j);
        rh_json_begin_obj(j);
        rh_json_key(j, "name");
        rh_json_str(j, name);
        rh_json_key(j, "type");
        rh_json_str(j, reg_type_name(type));
        rh_json_end_obj(j);
        ++i;
    }
    rh_json_end_arr(j);

    rh_json_end_obj(j);
    RegCloseKey(key);

    r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
}

/* --- registry.value.read ---------------------------------------------- */
/*
 * Input  : <path> <value>
 * Output : { "type": "REG_*", "data": "..." }
 *
 * Empty `value` ("") addresses the (Default) value via NULL ValueName.
 */
void rh_verb_registry_value_read(RhConn* c, const RhRequest* req)
{
    RhArgs      a;
    HKEY        key;
    LONG        rc;
    DWORD       type;
    DWORD       dlen;
    BYTE*       data;
    RhJson*     j;
    const char* r;
    const char* vname;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 2) {
        rh_err_msg(c, "invalid_args",
                   "registry.value.read requires <path> <value>");
        return;
    }
    rc = rh_classic_reg_open(a.pos[0], KEY_READ, &key);
    if (rc == ERROR_INVALID_PARAMETER) {
        rh_err_msg(c, "invalid_args", "unknown registry root");
        return;
    }
    if (rc != ERROR_SUCCESS) {
        rh_err(c, "not_found");
        return;
    }
    vname = default_value_translate(a.pos[1]);

    /* Size probe -> allocate -> read. */
    dlen = 0;
    rc = RegQueryValueExA(key, vname, NULL, &type, NULL, &dlen);
    if (rc != ERROR_SUCCESS) {
        RegCloseKey(key);
        rh_err(c, "not_found");
        return;
    }
    if (dlen == 0) {
        /* Empty payload still has a type. Allocate 1 byte for safety. */
        data = (BYTE*)malloc(1);
        if (data == NULL) {
            RegCloseKey(key);
            rh_err(c, "wire_desync");
            return;
        }
        data[0] = 0;
    } else {
        if (dlen > (DWORD)REG_DATA_READ_CAP) {
            RegCloseKey(key);
            rh_err_kv(c, "invalid_args", "reason", "value_too_large");
            return;
        }
        data = (BYTE*)malloc((size_t)dlen);
        if (data == NULL) {
            RegCloseKey(key);
            rh_err(c, "wire_desync");
            return;
        }
        rc = RegQueryValueExA(key, vname, NULL, &type, data, &dlen);
        if (rc != ERROR_SUCCESS) {
            free(data);
            RegCloseKey(key);
            rh_err(c, "not_found");
            return;
        }
    }
    RegCloseKey(key);

    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        free(data);
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);
    emit_value_body(j, type, data, dlen);
    r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
    free(data);
}

/* --- registry.value.create ------------------------------------------- */
/*
 * Input : <path> <value> <type> <data>
 * Errors: already_exists if the value is already present.
 *
 * Creates the parent key if missing (RegCreateKeyExA). Probes via
 * RegQueryValueExA before writing so existing values fail closed.
 */
void rh_verb_registry_value_create(RhConn* c, const RhRequest* req)
{
    RhArgs      a;
    HKEY        root;
    const char* subkey;
    HKEY        key;
    LONG        rc;
    DWORD       type;
    BYTE*       buf;
    DWORD       buflen;
    const char* vname;
    DWORD       probe_type;
    DWORD       probe_len;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 4) {
        rh_err_msg(c, "invalid_args",
                   "registry.value.create requires <path> <value> "
                   "<type> <data>");
        return;
    }
    if (!split_root(a.pos[0], &root, &subkey)) {
        rh_err_msg(c, "invalid_args", "unknown registry root");
        return;
    }
    if (!reg_type_from_name(a.pos[2], &type)) {
        rh_err_kv(c, "invalid_args", "reason", "unknown_type");
        return;
    }
    if (!build_value_data(type, a.pos[3], &buf, &buflen)) {
        rh_err_kv(c, "invalid_args", "reason", "bad_data");
        return;
    }
    rc = RegCreateKeyExA(root, subkey, 0, NULL, 0,
                         KEY_QUERY_VALUE | KEY_SET_VALUE,
                         NULL, &key, NULL);
    if (rc != ERROR_SUCCESS) {
        free(buf);
        rh_err(c, rh_win32_code((DWORD)rc));
        return;
    }
    vname = default_value_translate(a.pos[1]);

    /* Probe: refuse if the value already exists. */
    probe_len = 0;
    rc = RegQueryValueExA(key, vname, NULL, &probe_type, NULL, &probe_len);
    if (rc == ERROR_SUCCESS) {
        RegCloseKey(key);
        free(buf);
        rh_err(c, "already_exists");
        return;
    }

    rc = RegSetValueExA(key, vname, 0, type, buf, buflen);
    RegCloseKey(key);
    free(buf);
    if (rc != ERROR_SUCCESS) {
        if (rc == ERROR_INVALID_PARAMETER) {
            rh_err_kv(c, "invalid_args", "reason", "value_too_large");
        } else {
            rh_err(c, rh_win32_code((DWORD)rc));
        }
        return;
    }
    rh_ok(c);
}

/* --- registry.value.update ------------------------------------------- */
/*
 * Input : <path> <value> <type> <data>
 * Errors: not_found if the value (or key) is missing.
 *
 * Opens the key with QUERY+SET. Probes to require existence; never creates.
 */
void rh_verb_registry_value_update(RhConn* c, const RhRequest* req)
{
    RhArgs      a;
    HKEY        key;
    LONG        rc;
    DWORD       type;
    BYTE*       buf;
    DWORD       buflen;
    const char* vname;
    DWORD       probe_type;
    DWORD       probe_len;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 4) {
        rh_err_msg(c, "invalid_args",
                   "registry.value.update requires <path> <value> "
                   "<type> <data>");
        return;
    }
    if (!reg_type_from_name(a.pos[2], &type)) {
        rh_err_kv(c, "invalid_args", "reason", "unknown_type");
        return;
    }
    if (!build_value_data(type, a.pos[3], &buf, &buflen)) {
        rh_err_kv(c, "invalid_args", "reason", "bad_data");
        return;
    }
    rc = rh_classic_reg_open(a.pos[0],
                             KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (rc == ERROR_INVALID_PARAMETER) {
        free(buf);
        rh_err_msg(c, "invalid_args", "unknown registry root");
        return;
    }
    if (rc != ERROR_SUCCESS) {
        free(buf);
        rh_err(c, "not_found");
        return;
    }
    vname = default_value_translate(a.pos[1]);

    /* Probe: require the value to exist. */
    probe_len = 0;
    rc = RegQueryValueExA(key, vname, NULL, &probe_type, NULL, &probe_len);
    if (rc != ERROR_SUCCESS) {
        RegCloseKey(key);
        free(buf);
        rh_err(c, "not_found");
        return;
    }

    rc = RegSetValueExA(key, vname, 0, type, buf, buflen);
    RegCloseKey(key);
    free(buf);
    if (rc != ERROR_SUCCESS) {
        if (rc == ERROR_INVALID_PARAMETER) {
            rh_err_kv(c, "invalid_args", "reason", "value_too_large");
        } else {
            rh_err(c, rh_win32_code((DWORD)rc));
        }
        return;
    }
    rh_ok(c);
}

/* --- registry.value.delete ------------------------------------------- */
/*
 * Input : <path> <value>
 * Errors: not_found if the key or value is missing.
 */
void rh_verb_registry_value_delete(RhConn* c, const RhRequest* req)
{
    RhArgs      a;
    HKEY        key;
    LONG        rc;
    const char* vname;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 2) {
        rh_err_msg(c, "invalid_args",
                   "registry.value.delete requires <path> <value>");
        return;
    }
    rc = rh_classic_reg_open(a.pos[0], KEY_SET_VALUE, &key);
    if (rc == ERROR_INVALID_PARAMETER) {
        rh_err_msg(c, "invalid_args", "unknown registry root");
        return;
    }
    if (rc != ERROR_SUCCESS) {
        rh_err(c, "not_found");
        return;
    }
    vname = default_value_translate(a.pos[1]);
    rc = RegDeleteValueA(key, vname);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        rh_err(c, (rc == ERROR_FILE_NOT_FOUND) ? "not_found"
                                               : rh_win32_code((DWORD)rc));
        return;
    }
    rh_ok(c);
}

/* --- registry.key.delete -- recursive walk helper ---------------------- */

/* Pre-order walk that deletes every subkey of `parent_path` before
 * deleting `parent_path` itself. classic (NT 4 / 9x) has no RegDeleteTree
 * and no RegDeleteKeyExA -- the emulation is mandatory.
 *
 * `parent_path` is a hive-rooted path ("HKLM\\Software\\X"); enumerated
 * child names are appended with a '\\' separator to form the next call.
 *
 * Returns ERROR_SUCCESS on full success, the first Win32 error otherwise. */
static LONG reg_delete_tree(const char* parent_path)
{
    HKEY  key;
    LONG  rc;
    char  name[512];
    DWORD nlen;
    char  child[1024];
    HKEY  root;
    const char* subkey;
    int   plen;

    rc = rh_classic_reg_open(parent_path,
                             KEY_READ | KEY_WRITE, &key);
    if (rc != ERROR_SUCCESS) {
        return rc;
    }

    /* Enumerate-and-delete loop: always read index 0 because each
     * successful subtree-delete shifts the remaining children down. */
    for (;;) {
        nlen = (DWORD)sizeof(name);
        rc = RegEnumKeyExA(key, 0, name, &nlen,
                           NULL, NULL, NULL, NULL);
        if (rc == ERROR_NO_MORE_ITEMS) {
            rc = ERROR_SUCCESS;
            break;
        }
        if (rc != ERROR_SUCCESS) {
            break;
        }
        plen = (int)strlen(parent_path);
        if (plen + 1 + (int)nlen + 1 > (int)sizeof(child)) {
            rc = ERROR_BUFFER_OVERFLOW;
            break;
        }
        memcpy(child, parent_path, (size_t)plen);
        child[plen] = '\\';
        memcpy(child + plen + 1, name, (size_t)nlen + 1);
        rc = reg_delete_tree(child);
        if (rc != ERROR_SUCCESS) {
            break;
        }
    }
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        return rc;
    }

    /* Now the parent is empty (or was). Delete it via RegDeleteKeyA on
     * the hive root. */
    if (!split_root(parent_path, &root, &subkey)) {
        return ERROR_INVALID_PARAMETER;
    }
    return RegDeleteKeyA(root, subkey);
}

/* --- registry.key.delete --------------------------------------------- */
/*
 * Input  : <path> [--recursive]
 * Output : OK 0
 * Errors : not_found  -- key missing
 *          not_empty  -- key has subkeys and --recursive not set
 */
void rh_verb_registry_key_delete(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = { { "--recursive", 0 } };
    RhArgs      a;
    HKEY        root;
    const char* subkey;
    LONG        rc;
    HKEY        probe;

    rh_args_parse(req, defs, 1, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "registry.key.delete requires <path>");
        return;
    }
    if (!split_root(a.pos[0], &root, &subkey)) {
        rh_err_msg(c, "invalid_args", "unknown registry root");
        return;
    }

    /* Confirm the key exists so we can distinguish not_found from
     * permission_denied / other errors emitted by RegDeleteKeyA. */
    rc = RegOpenKeyExA(root, subkey, 0, KEY_READ, &probe);
    if (rc != ERROR_SUCCESS) {
        rh_err(c, (rc == ERROR_FILE_NOT_FOUND ||
                   rc == ERROR_PATH_NOT_FOUND)
                  ? "not_found" : rh_win32_code((DWORD)rc));
        return;
    }
    RegCloseKey(probe);

    if (a.seen[0]) {
        /* --recursive : walk + delete. */
        rc = reg_delete_tree(a.pos[0]);
    } else {
        /* Non-recursive: RegDeleteKeyA refuses non-empty keys on NT,
         * returning ERROR_ACCESS_DENIED. Probe for subkeys first so the
         * caller gets the spec-mandated `not_empty` code rather than a
         * generic permission_denied. */
        HKEY  k;
        DWORD nsubs = 0;
        char  tmp[256];
        DWORD tlen = (DWORD)sizeof(tmp);
        rc = RegOpenKeyExA(root, subkey, 0, KEY_READ, &k);
        if (rc == ERROR_SUCCESS) {
            LONG erc = RegEnumKeyExA(k, 0, tmp, &tlen,
                                     NULL, NULL, NULL, NULL);
            /* ERROR_MORE_DATA -> name too long for tmp[], but a child
             * exists. Treat as "has subkeys" so we don't false-negative. */
            if (erc == ERROR_SUCCESS || erc == ERROR_MORE_DATA) {
                nsubs = 1;
            }
            RegCloseKey(k);
        }
        if (nsubs > 0) {
            rh_err(c, "not_empty");
            return;
        }
        rc = RegDeleteKeyA(root, subkey);
    }
    if (rc != ERROR_SUCCESS) {
        if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND) {
            rh_err(c, "not_found");
        } else if (rc == ERROR_ACCESS_DENIED) {
            /* On classic, non-empty + non-recursive falls through to
             * RegDeleteKeyA which returns ACCESS_DENIED; the probe above
             * should have caught the not_empty case. Map any remaining
             * ACCESS_DENIED to permission_denied per common.c. */
            rh_err(c, "permission_denied");
        } else {
            rh_err(c, rh_win32_code((DWORD)rc));
        }
        return;
    }
    rh_ok(c);
}

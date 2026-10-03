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

#include "encoding.h"

#include <stdlib.h>
#include <string.h>

/* cp1252 0x80..0x9F -> Unicode (0 = undefined in cp1252). */
static const unsigned short kCp1252Hi[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178
};

static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int rh_enc_parse(const char* name)
{
    if (name == NULL)                    return RH_ENC_UTF8;
    if (strcmp(name, "utf-8") == 0)     return RH_ENC_UTF8;
    if (strcmp(name, "utf-16le") == 0)  return RH_ENC_UTF16LE;
    if (strcmp(name, "utf-16be") == 0)  return RH_ENC_UTF16BE;
    if (strcmp(name, "ascii") == 0)     return RH_ENC_ASCII;
    if (strcmp(name, "latin-1") == 0)   return RH_ENC_LATIN1;
    if (strcmp(name, "cp1252") == 0)    return RH_ENC_CP1252;
    if (strcmp(name, "binary") == 0)    return RH_ENC_BINARY;
    return -1;
}

const char* rh_enc_name(int enc)
{
    switch (enc) {
        case RH_ENC_UTF16LE: return "utf-16le";
        case RH_ENC_UTF16BE: return "utf-16be";
        case RH_ENC_ASCII:   return "ascii";
        case RH_ENC_LATIN1:  return "latin-1";
        case RH_ENC_CP1252:  return "cp1252";
        case RH_ENC_BINARY:  return "binary";
        default:             return "utf-8";
    }
}

/* --- UTF-8 primitives -------------------------------------------------- */

/* Decode one UTF-8 sequence at s[*i] (n bytes total). Returns the code
 * point, or -1 for an invalid sequence (one byte consumed). */
static long utf8_next(const unsigned char* s, int n, int* i)
{
    unsigned c = s[*i];
    long     cp;
    int      extra;
    int      k;

    if (c < 0x80) {
        *i += 1;
        return (long)c;
    }
    if      ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { *i += 1; return -1; }

    if (*i + extra >= n) {          /* truncated sequence */
        *i += 1;
        return -1;
    }
    for (k = 1; k <= extra; ++k) {
        if ((s[*i + k] & 0xC0) != 0x80) {
            *i += 1;
            return -1;
        }
        cp = (cp << 6) | (s[*i + k] & 0x3F);
    }
    /* Reject overlongs, surrogates and out-of-range values. */
    if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
        (extra == 3 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
        *i += 1;
        return -1;
    }
    *i += extra + 1;
    return cp;
}

static int utf8_put(char* out, long cp)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* --- base64 ------------------------------------------------------------ */

char* rh_base64_encode(const char* data, int len, int* out_len)
{
    const unsigned char* p = (const unsigned char*)data;
    char*                out;
    int                  o;
    int                  i;
    unsigned long        v;

    out = (char*)malloc((size_t)(((len + 2) / 3) * 4 + 1));
    if (out == NULL) {
        return NULL;
    }
    o = 0;
    for (i = 0; i + 2 < len; i += 3) {
        v = ((unsigned long)p[i] << 16) | ((unsigned long)p[i + 1] << 8) |
            p[i + 2];
        out[o++] = kB64[(v >> 18) & 63];
        out[o++] = kB64[(v >> 12) & 63];
        out[o++] = kB64[(v >> 6) & 63];
        out[o++] = kB64[v & 63];
    }
    if (len - i == 1) {
        v = (unsigned long)p[i] << 16;
        out[o++] = kB64[(v >> 18) & 63];
        out[o++] = kB64[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (len - i == 2) {
        v = ((unsigned long)p[i] << 16) | ((unsigned long)p[i + 1] << 8);
        out[o++] = kB64[(v >> 18) & 63];
        out[o++] = kB64[(v >> 12) & 63];
        out[o++] = kB64[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = '\0';
    if (out_len != NULL) {
        *out_len = o;
    }
    return out;
}

static int b64_val(int ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static int base64_decode(const char* in, int len, char** out, int* out_len)
{
    char*         buf;
    int           o;
    int           i;
    int           q;
    int           pad;
    int           v;
    unsigned long acc;

    buf = (char*)malloc((size_t)(len / 4 * 3 + 3));
    if (buf == NULL) {
        return 0;
    }
    o = 0; q = 0; pad = 0; acc = 0;
    for (i = 0; i < len; ++i) {
        int ch = (unsigned char)in[i];
        if (ch == '\r' || ch == '\n' || ch == ' ') {
            continue;
        }
        if (ch == '=') {
            pad += 1;
            v = 0;
        } else {
            if (pad > 0) { free(buf); return 0; }   /* data after '=' */
            v = b64_val(ch);
            if (v < 0)   { free(buf); return 0; }
        }
        acc = (acc << 6) | (unsigned long)v;
        if (++q == 4) {
            buf[o++] = (char)((acc >> 16) & 0xFF);
            if (pad < 2) buf[o++] = (char)((acc >> 8) & 0xFF);
            if (pad < 1) buf[o++] = (char)(acc & 0xFF);
            q = 0;
            acc = 0;
        }
    }
    if (q != 0 || pad > 2) {
        free(buf);
        return 0;
    }
    *out     = buf;
    *out_len = o;
    return 1;
}

/* --- encode: wire text -> disk bytes ----------------------------------- */

int rh_enc_encode(int enc, const char* text, int len,
                  char** out, int* out_len)
{
    const unsigned char* s = (const unsigned char*)text;
    char*                buf;
    int                  o;
    int                  i;
    long                 cp;
    int                  k;

    *out     = NULL;
    *out_len = 0;
    if (enc == RH_ENC_BINARY) {
        return base64_decode(text, len, out, out_len);
    }
    if (len == 0) {
        return 1;
    }
    if (enc == RH_ENC_UTF8) {
        buf = (char*)malloc((size_t)len);
        if (buf == NULL) return 0;
        memcpy(buf, text, (size_t)len);
        *out = buf;
        *out_len = len;
        return 1;
    }

    /* Worst case: UTF-16 needs 4 bytes per 1-byte input char. */
    buf = (char*)malloc((size_t)len * 4);
    if (buf == NULL) {
        return 0;
    }
    o = 0;
    i = 0;
    while (i < len) {
        cp = utf8_next(s, len, &i);
        if (cp < 0) {
            free(buf);
            return 0;
        }
        switch (enc) {
            case RH_ENC_UTF16LE:
            case RH_ENC_UTF16BE: {
                unsigned u[2];
                int      nu;
                int      be = (enc == RH_ENC_UTF16BE);
                if (cp >= 0x10000) {
                    u[0] = (unsigned)(0xD800 + ((cp - 0x10000) >> 10));
                    u[1] = (unsigned)(0xDC00 + ((cp - 0x10000) & 0x3FF));
                    nu = 2;
                } else {
                    u[0] = (unsigned)cp;
                    nu = 1;
                }
                for (k = 0; k < nu; ++k) {
                    buf[o++] = (char)(be ? (u[k] >> 8) : (u[k] & 0xFF));
                    buf[o++] = (char)(be ? (u[k] & 0xFF) : (u[k] >> 8));
                }
                break;
            }
            case RH_ENC_ASCII:
                if (cp > 0x7F) { free(buf); return 0; }
                buf[o++] = (char)cp;
                break;
            case RH_ENC_LATIN1:
                if (cp > 0xFF) { free(buf); return 0; }
                buf[o++] = (char)cp;
                break;
            default: /* RH_ENC_CP1252 */
                if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) {
                    buf[o++] = (char)cp;
                } else {
                    for (k = 0; k < 32; ++k) {
                        if (kCp1252Hi[k] != 0 && kCp1252Hi[k] == cp) {
                            break;
                        }
                    }
                    if (k == 32) { free(buf); return 0; }
                    buf[o++] = (char)(0x80 + k);
                }
                break;
        }
    }
    *out     = buf;
    *out_len = o;
    return 1;
}

/* --- decode: disk bytes -> wire text ----------------------------------- */

int rh_enc_decode(int enc, const char* bytes, int len,
                  char** out, int* out_len)
{
    const unsigned char* s = (const unsigned char*)bytes;
    char*                buf;
    int                  o;
    int                  i;
    long                 cp;

    if (enc == RH_ENC_BINARY) {
        *out = rh_base64_encode(bytes, len, out_len);
        return (*out != NULL) ? 1 : 0;
    }

    /* Worst case 4 UTF-8 bytes per input byte (U+FFFD is 3). */
    buf = (char*)malloc((size_t)len * 4 + 1);
    if (buf == NULL) {
        return 0;
    }
    o = 0;
    i = 0;
    while (i < len) {
        switch (enc) {
            case RH_ENC_UTF8:
                cp = utf8_next(s, len, &i);
                break;
            case RH_ENC_UTF16LE:
            case RH_ENC_UTF16BE: {
                int      be = (enc == RH_ENC_UTF16BE);
                unsigned u;
                unsigned u2;
                if (i + 1 >= len) {
                    cp = -1;
                    i = len;
                    break;
                }
                u = be ? ((unsigned)s[i] << 8 | s[i + 1])
                       : ((unsigned)s[i + 1] << 8 | s[i]);
                i += 2;
                if (u >= 0xD800 && u <= 0xDBFF && i + 1 < len) {
                    u2 = be ? ((unsigned)s[i] << 8 | s[i + 1])
                            : ((unsigned)s[i + 1] << 8 | s[i]);
                    if (u2 >= 0xDC00 && u2 <= 0xDFFF) {
                        i += 2;
                        cp = 0x10000 + (((long)u - 0xD800) << 10) +
                             ((long)u2 - 0xDC00);
                        break;
                    }
                }
                cp = (u >= 0xD800 && u <= 0xDFFF) ? -1 : (long)u;
                break;
            }
            case RH_ENC_ASCII:
                cp = (s[i] < 0x80) ? (long)s[i] : -1;
                i += 1;
                break;
            case RH_ENC_LATIN1:
                cp = (long)s[i];
                i += 1;
                break;
            default: /* RH_ENC_CP1252 */
                if (s[i] >= 0x80 && s[i] <= 0x9F) {
                    cp = kCp1252Hi[s[i] - 0x80];
                    if (cp == 0) cp = -1;
                } else {
                    cp = (long)s[i];
                }
                i += 1;
                break;
        }
        o += utf8_put(buf + o, (cp < 0) ? 0xFFFDL : cp);
    }
    buf[o] = '\0';
    *out     = buf;
    *out_len = o;
    return 1;
}

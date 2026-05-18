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

#include "json.h"

#include <stdio.h>
#include <string.h>

/* --- low-level append (overflow-checked) ------------------------------- */

static void j_putn(RhJson* j, const char* s, int n)
{
    if (j->error) {
        return;
    }
    if (j->len + n > RH_MAX_JSON_LEN - 1) {
        j->error = 1;
        return;
    }
    memcpy(j->buf + j->len, s, (size_t)n);
    j->len += n;
}

static void j_puts(RhJson* j, const char* s)
{
    j_putn(j, s, (int)strlen(s));
}

static void j_putc(RhJson* j, char c)
{
    j_putn(j, &c, 1);
}

/* Append `s` as a JSON string literal, including the surrounding quotes,
 * escaping per RFC 8259. */
static void j_quoted(RhJson* j, const char* s)
{
    static const char kHex[] = "0123456789abcdef";
    unsigned char c;
    int i;

    j_putc(j, '"');
    if (s != NULL) {
        for (i = 0; s[i] != '\0'; ++i) {
            c = (unsigned char)s[i];
            switch (c) {
                case '"':  j_puts(j, "\\\""); break;
                case '\\': j_puts(j, "\\\\"); break;
                case '\b': j_puts(j, "\\b");  break;
                case '\f': j_puts(j, "\\f");  break;
                case '\n': j_puts(j, "\\n");  break;
                case '\r': j_puts(j, "\\r");  break;
                case '\t': j_puts(j, "\\t");  break;
                default:
                    if (c < 0x20) {
                        char esc[6];
                        esc[0] = '\\'; esc[1] = 'u';
                        esc[2] = '0';  esc[3] = '0';
                        esc[4] = kHex[(c >> 4) & 0x0f];
                        esc[5] = kHex[c & 0x0f];
                        j_putn(j, esc, 6);
                    } else {
                        j_putc(j, (char)c);
                    }
                    break;
            }
        }
    }
    j_putc(j, '"');
}

/* Emit the inter-member comma for the current container, unless this is the
 * first member. Call once before each object key and each array element. */
static void j_separator(RhJson* j)
{
    if (j->depth <= 0 || j->depth > RH_JSON_MAX_DEPTH) {
        return;
    }
    if (j->first[j->depth - 1]) {
        j->first[j->depth - 1] = 0;
    } else {
        j_putc(j, ',');
    }
}

static void j_open(RhJson* j, char bracket)
{
    if (j->depth >= RH_JSON_MAX_DEPTH) {
        j->error = 1;
        return;
    }
    j_putc(j, bracket);
    j->first[j->depth] = 1;
    j->depth += 1;
}

static void j_close(RhJson* j, char bracket)
{
    if (j->depth <= 0) {
        j->error = 1;
        return;
    }
    j->depth -= 1;
    j_putc(j, bracket);
}

/* --- public API -------------------------------------------------------- */

void rh_json_init(RhJson* j)
{
    j->len = 0;
    j->error = 0;
    j->depth = 0;
    j->buf[0] = '\0';
}

void rh_json_begin_obj(RhJson* j) { j_open(j, '{'); }
void rh_json_end_obj(RhJson* j)   { j_close(j, '}'); }
void rh_json_begin_arr(RhJson* j) { j_open(j, '['); }
void rh_json_end_arr(RhJson* j)   { j_close(j, ']'); }

void rh_json_key(RhJson* j, const char* key)
{
    j_separator(j);
    j_quoted(j, key);
    j_putc(j, ':');
}

void rh_json_str(RhJson* j, const char* s)
{
    j_quoted(j, s);
}

void rh_json_int(RhJson* j, i32 v)
{
    char num[16];
    _snprintf(num, sizeof(num), "%d", (int)v);
    num[sizeof(num) - 1] = '\0';
    j_puts(j, num);
}

void rh_json_bool(RhJson* j, int v)
{
    j_puts(j, v ? "true" : "false");
}

void rh_json_null(RhJson* j)
{
    j_puts(j, "null");
}

void rh_json_raw(RhJson* j, const char* raw_json)
{
    j_puts(j, raw_json);
}

void rh_json_arr_str(RhJson* j, const char* s)
{
    j_separator(j);
    j_quoted(j, s);
}

void rh_json_arr_elem(RhJson* j)
{
    j_separator(j);
}

const char* rh_json_finish(RhJson* j)
{
    if (j->error || j->depth != 0) {
        return NULL;
    }
    j->buf[j->len] = '\0';
    return j->buf;
}

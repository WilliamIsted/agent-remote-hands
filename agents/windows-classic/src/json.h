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

/* Minimal C89 JSON builder for response payloads.
 *
 * No parser: the v2.1 wire verbs implemented in W8 take no JSON request
 * bodies. Supports nested objects and string arrays because system.info
 * (PROTOCOL.md 3.1) and system.capabilities (3.2) are inherently nested
 * (the W8 prompt's "keep it flat" guidance does not match the real
 * conformance contract -- see the W8 report deviations note).
 *
 * Comma placement is automatic: a per-depth "first member" flag stack
 * suppresses the leading comma for the first member of each container. */

#ifndef RH_JSON_H
#define RH_JSON_H

#include "types.h"

#define RH_JSON_MAX_DEPTH 8

typedef struct {
    char buf[RH_MAX_JSON_LEN];
    int  len;
    int  error;                       /* 1 on overflow or structural misuse */
    int  depth;                       /* open-container depth                */
    int  first[RH_JSON_MAX_DEPTH];    /* first[d]=1 => no leading comma yet  */
} RhJson;

void rh_json_init(RhJson* j);

void rh_json_begin_obj(RhJson* j);
void rh_json_end_obj(RhJson* j);
void rh_json_begin_arr(RhJson* j);
void rh_json_end_arr(RhJson* j);

/* Object member key: emits the separating comma (if needed) then "key": .
 * The caller then emits exactly one value via a value emitter below. */
void rh_json_key(RhJson* j, const char* key);

/* Value emitters (use immediately after rh_json_key). No punctuation. */
void rh_json_str(RhJson* j, const char* s);
void rh_json_int(RhJson* j, i32 v);
void rh_json_bool(RhJson* j, int v);
void rh_json_null(RhJson* j);
void rh_json_raw(RhJson* j, const char* raw_json);

/* Array element: emits the separating comma (if needed) then the value. */
void rh_json_arr_str(RhJson* j, const char* s);

/* Emit only the inter-element separator for the current array (comma unless
 * this is the first element). Call immediately before rh_json_begin_obj /
 * rh_json_begin_arr / rh_json_int / rh_json_bool when that value is an
 * element of an enclosing array. (rh_json_arr_str already does this
 * internally for string elements; this is the general form W9 needs for
 * arrays of objects -- window.list, process.list, directory.list, ...). */
void rh_json_arr_elem(RhJson* j);

/* Returns the finished buffer, or NULL if the document overflowed or the
 * containers were left unbalanced. */
const char* rh_json_finish(RhJson* j);

#endif /* RH_JSON_H */

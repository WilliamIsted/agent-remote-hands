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

/* Foundational typedefs and limits for the windows-classic agent.
 *
 * C89 / VS6 has no <stdint.h>; the MSVC __intNN extension is the portable
 * fixed-width route on this toolchain. Everything else in the classic tree
 * includes this header first. */

#ifndef RH_TYPES_H
#define RH_TYPES_H

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned __int64    u64;
typedef int                 i32;
typedef __int64             i64;

#define RH_TRUE   1
#define RH_FALSE  0

/* Wire limits. Header line cap matches PROTOCOL.md 1.2 (65535 bytes). */
#define RH_MAX_HEADER_LEN  65535
#define RH_MAX_VERB_LEN       64
#define RH_MAX_ARG_LEN       512
#define RH_MAX_ARGS            8

/* JSON response buffer. W8 sized this 8192 for the three system.* verbs.
 * W9 returns directory.list / process.list / window.list / registry.read
 * payloads that routinely exceed 8 KB (a C:\Windows listing is ~12 KB), so
 * the cap is raised. RhJson is heap-allocated by the list handlers (see
 * verbs/common.h rh_json_heap_*) to keep this off the stack. */
#define RH_MAX_JSON_LEN    65536

#define RH_PORT_DEFAULT     8765

/* 256-bit elevation token, hex-encoded (PROTOCOL.md 2.6). */
#define RH_TOKEN_BYTES        32
#define RH_TOKEN_HEX_LEN      64

#endif /* RH_TYPES_H */

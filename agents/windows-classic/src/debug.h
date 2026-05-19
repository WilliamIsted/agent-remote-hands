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

/* Compile-time debug wire-trace sink (RH_DEBUG builds only).
 *
 * rh_debug_open() -- call once after WSAStartup; opens debug.log for appending
 * rh_dbg(fmt, ...) -- printf-style; writes a line to both stderr and debug.log
 * g_rh_dbg_fp     -- NULL until rh_debug_open() succeeds or if fopen fails
 */

#ifndef RH_DEBUG_H
#define RH_DEBUG_H

#ifdef RH_DEBUG

#include <stdio.h>

extern FILE* g_rh_dbg_fp;

void rh_debug_open(void);
void rh_dbg(const char* fmt, ...);

#endif /* RH_DEBUG */

#endif /* RH_DEBUG_H */

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

#include "debug.h"

#ifdef RH_DEBUG

#include <stdio.h>
#include <stdarg.h>

FILE* g_rh_dbg_fp = NULL;

void rh_debug_open(void)
{
    g_rh_dbg_fp = fopen("debug.log", "a");
}

/* Write a formatted line to stderr and (if open) to debug.log.
 * C89 has no va_copy; calling va_start twice on separate va_list variables
 * is the portable workaround to pass variadic args to two vfprintf calls. */
void rh_dbg(const char* fmt, ...)
{
    va_list args;
    va_list args2;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);

    if (g_rh_dbg_fp != NULL) {
        va_start(args2, fmt);
        vfprintf(g_rh_dbg_fp, fmt, args2);
        fprintf(g_rh_dbg_fp, "\n");
        fflush(g_rh_dbg_fp);
        va_end(args2);
    }
}

#else

/* Suppress C4206: nonstandard extension used -- translation unit is empty */
static int rh_debug_placeholder = 0;

#endif /* RH_DEBUG */

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

/* file.* (PROTOCOL.md 4.6). ANSI Win32 (CreateFileA/FindFirstFileA), the
 * whole NT4/Win9x/Win2000 range. directory-only verbs live in directory.c. */

#ifndef RH_VERBS_FILE_H
#define RH_VERBS_FILE_H

#include "../connection.h"

void rh_verb_file_read(RhConn* c, const RhRequest* req);
void rh_verb_file_write(RhConn* c, const RhRequest* req);
void rh_verb_file_write_at(RhConn* c, const RhRequest* req);
void rh_verb_file_stat(RhConn* c, const RhRequest* req);
void rh_verb_file_delete(RhConn* c, const RhRequest* req);
void rh_verb_file_exists(RhConn* c, const RhRequest* req);
void rh_verb_file_wait(RhConn* c, const RhRequest* req);
void rh_verb_file_rename(RhConn* c, const RhRequest* req);

/* v2.1 Create-tier: refuse-if-exists, atomic temp+rename by default.
 *    file.create <path> <length> [--encoding <enc>] [--no-atomic]
 * Encoding: utf-8 (default) | binary | utf-16le | utf-16be | cp1252
 *           | ascii | latin-1. utf-8/binary write payload bytes as-is;
 * the rest transcode UTF-8 payload -> wide -> target code page. */
void rh_verb_file_create(RhConn* c, const RhRequest* req);

/* v2.0 HTTP(S)-to-disk via WinINet (NT 4 / 9x / 2000+).
 *    file.download <url> <path> [--timeout-ms <ms>] [--no-verify-tls] */
void rh_verb_file_download(RhConn* c, const RhRequest* req);

#endif /* RH_VERBS_FILE_H */

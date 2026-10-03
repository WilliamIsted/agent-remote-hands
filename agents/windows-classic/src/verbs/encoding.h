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

/* Text encodings for file.read / file.write / file.write_at / file.create
 * (spec enum: utf-8, utf-16le, utf-16be, ascii, latin-1, cp1252, binary).
 * Hand-rolled rather than MultiByteToWideChar so behaviour is identical on
 * NT 4 / 9x, where the code-page tables vary by install. Wire-side text is
 * always UTF-8; `binary` is base64 on the wire. */

#ifndef RH_VERBS_ENCODING_H
#define RH_VERBS_ENCODING_H

#define RH_ENC_UTF8     0
#define RH_ENC_UTF16LE  1
#define RH_ENC_UTF16BE  2
#define RH_ENC_ASCII    3
#define RH_ENC_LATIN1   4
#define RH_ENC_CP1252   5
#define RH_ENC_BINARY   6

/* Spec name -> RH_ENC_*; NULL means the default (utf-8). -1 if unknown. */
int         rh_enc_parse(const char* name);
const char* rh_enc_name(int enc);

/* Wire text (UTF-8, or base64 for binary) -> on-disk bytes. On success
 * returns 1 and a malloc'd *out (may be NULL when *out_len is 0). Returns
 * 0 when the text is not representable in `enc` (e.g. non-ASCII under
 * ascii, bad base64) -- the caller replies ERR invalid_args. */
int rh_enc_encode(int enc, const char* text, int len,
                  char** out, int* out_len);

/* On-disk bytes -> wire text (UTF-8, or base64 for binary). Invalid input
 * sequences decode to U+FFFD so the result is always valid UTF-8. Returns
 * 1 and a malloc'd, NUL-terminated *out, or 0 on allocation failure. */
int rh_enc_decode(int enc, const char* bytes, int len,
                  char** out, int* out_len);

/* Standard base64 (RFC 4648, padded). Returns malloc'd NUL-terminated text,
 * or NULL on allocation failure. */
char* rh_base64_encode(const char* data, int len, int* out_len);

#endif /* RH_VERBS_ENCODING_H */

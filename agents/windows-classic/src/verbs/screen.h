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

/* screen.capture (PROTOCOL.md 4.2). GDI BitBlt capture; BMP and a minimal
 * stored-DEFLATE PNG encoder (no GDI+, no libpng -- D10: "BMP always
 * available; no GDI+/UIA bundling"). PNG support is added because the
 * conformance contract (test_screen.py test_screen_capture_png_signature)
 * asserts a real PNG magic for `--format png`; a hand-rolled uncompressed
 * PNG keeps D10's "no GDI+ bundling" intent (it links nothing extra). */

#ifndef RH_VERBS_SCREEN_H
#define RH_VERBS_SCREEN_H

#include "../connection.h"

void rh_verb_screen_capture(RhConn* c, const RhRequest* req);

#endif /* RH_VERBS_SCREEN_H */

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

/* input.* (PROTOCOL.md 4.4). Classic uses mouse_event / keybd_event +
 * VkKeyScanA (SendInput + KEYEVENTF_UNICODE is Win2000+ and is the
 * modern/legacy path -- see the plan's seam discussion). All input verbs
 * are update tier; the dispatch table gates that before these run. */

#ifndef RH_VERBS_INPUT_H
#define RH_VERBS_INPUT_H

#include "../connection.h"

void rh_verb_input_click(RhConn* c, const RhRequest* req);
void rh_verb_input_move(RhConn* c, const RhRequest* req);
void rh_verb_input_scroll(RhConn* c, const RhRequest* req);
void rh_verb_input_key(RhConn* c, const RhRequest* req);
void rh_verb_input_type(RhConn* c, const RhRequest* req);
void rh_verb_input_send_message(RhConn* c, const RhRequest* req);
void rh_verb_input_post_message(RhConn* c, const RhRequest* req);

/* Phase R2 extended input verbs (v2.2): the existing click/key/type are
 * composite (down+up) shortcuts; these decompose so the caller can hold a
 * button or key across multiple requests (drag-and-drop, modifier-hold,
 * game input). All use SendInput when present (XP+ / Win2000+ via the
 * platform SDK) and fall back to mouse_event / keybd_event on NT 4 / 9x. */
void rh_verb_input_mouse_press(RhConn* c, const RhRequest* req);
void rh_verb_input_mouse_release(RhConn* c, const RhRequest* req);
void rh_verb_input_mouse_drag(RhConn* c, const RhRequest* req);
void rh_verb_input_keyboard_key_down(RhConn* c, const RhRequest* req);
void rh_verb_input_keyboard_key_up(RhConn* c, const RhRequest* req);
void rh_verb_input_position(RhConn* c, const RhRequest* req);

#endif /* RH_VERBS_INPUT_H */

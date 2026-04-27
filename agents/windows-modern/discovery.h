#pragma once
/* discovery.h — mDNS / DNS-SD responder so AI controllers can find the
 * agent on the LAN without the operator looking up the VM's IP.
 *
 * Service type: _remote-hands._tcp.local.
 * Service name: <hostname>._remote-hands._tcp.local.
 *
 * TXT records advertise a small subset of INFO so a scanner can pre-filter
 * agents without opening a full TCP session:
 *   protocol=1
 *   os=windows-modern
 *   power=no
 *   version=...
 *
 * Opt-in by design: the agent has no auth (per PROTOCOL.md the trust
 * boundary is the network), so broadcasting its presence is a footgun on
 * untrusted networks. Default off; enable via REMOTE_HANDS_DISCOVERABLE=1
 * or --discoverable.
 */

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the mDNS responder thread. Best-effort — failure to bind the
   multicast socket is logged and the agent runs normally without
   discovery. service_port is the agent's TCP port; os_name is the value
   for the os= TXT record. Returns 1 on success, 0 on failure. */
int discovery_start(unsigned short service_port, const char *os_name);

/* Signal the responder thread to exit and join it. Idempotent. */
void discovery_stop(void);

/* True after a successful discovery_start(). */
int discovery_active(void);

#ifdef __cplusplus
}
#endif

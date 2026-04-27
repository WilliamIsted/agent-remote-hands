/* discovery.cpp — minimal mDNS / DNS-SD responder for the windows-modern
 * agent. Handles the common case: a scanner (Avahi, Bonjour, dns-sd,
 * Python zeroconf, ...) sends a PTR query for `_remote-hands._tcp.local.`
 * via multicast 224.0.0.251:5353; we reply with the four records it needs
 * to connect — PTR, SRV, TXT, A — in one response packet.
 *
 * Deliberately simplified vs. a full mDNS stack:
 *   - IPv4 only.
 *   - One network interface (the first non-loopback IPv4 we find).
 *   - No conflict-resolution dance; the instance name is just the host.
 *   - No TTL refresh / announcement schedule; respond on demand.
 *   - No name compression in our outbound packets (wastes bytes, doesn't
 *     change correctness).
 *
 * Opt-in: the agent only starts the responder when started with
 * REMOTE_HANDS_DISCOVERABLE=1 (or --discoverable). Default off — the
 * protocol has no auth, broadcasting on an untrusted network is a
 * footgun.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "discovery.h"

#pragma comment(lib, "iphlpapi.lib")

#define MDNS_PORT     5353
#define MDNS_GROUP    "224.0.0.251"
#define MDNS_TTL      120u

#define DNS_TYPE_A    1
#define DNS_TYPE_PTR  12
#define DNS_TYPE_TXT  16
#define DNS_TYPE_SRV  33
#define DNS_TYPE_ANY  255
#define DNS_CLASS_IN  1
#define DNS_FLUSH_BIT 0x8000

struct discovery_state {
    SOCKET sock = INVALID_SOCKET;
    HANDLE thread = NULL;
    volatile LONG running = 0;
    char host_label[64];          /* "WINDEV-VM" — bare, no .local. */
    char service_name[128];       /* "_remote-hands._tcp.local."   */
    char instance_name[256];      /* "<host>._remote-hands._tcp.local." */
    char host_dotted[80];         /* "<host>.local." */
    char txt_payload[256];        /* concatenated <len><kv><len><kv>... */
    int  txt_payload_len = 0;
    unsigned long ipv4_addr_n = 0; /* network byte order */
    unsigned short port = 0;
};

static discovery_state g_disc;
static int g_started = 0;

extern "C" int discovery_active(void) { return g_started; }

/* ------------------------------------------------------------------ */
/* DNS-name encoding: turn "_remote-hands._tcp.local." into the wire
   format [13]"_remote-hands"[4]"_tcp"[5]"local"[0]. Returns bytes
   written, or -1 on overflow.                                         */
/* ------------------------------------------------------------------ */

static int encode_dns_name(const char *dotted, unsigned char *out, int max) {
    int outpos = 0;
    const char *p = dotted;
    while (*p) {
        const char *dot = strchr(p, '.');
        int label_len = dot ? (int)(dot - p) : (int)strlen(p);
        if (label_len == 0) { p++; continue; }
        if (label_len > 63 || outpos + 1 + label_len + 1 > max) return -1;
        out[outpos++] = (unsigned char)label_len;
        memcpy(out + outpos, p, label_len);
        outpos += label_len;
        if (!dot) break;
        p = dot + 1;
    }
    if (outpos >= max) return -1;
    out[outpos++] = 0;
    return outpos;
}

/* Decode a possibly-compressed DNS name from `pkt` starting at `offset`.
   Writes the dotted form into `out` (with trailing dot retained, all
   lowercase). Returns the new offset *past* the name (or -1 on error).
   For pointers, the offset advances past the 2-byte pointer; the dotted
   string follows the pointer chain. */
static int decode_dns_name(const unsigned char *pkt, int pkt_len, int offset,
                           char *out, int max) {
    int outpos = 0;
    int orig_offset = offset;
    int hops = 0;
    int final_offset = -1;
    while (offset < pkt_len) {
        unsigned char b = pkt[offset];
        if (b == 0) {
            offset++;
            if (final_offset < 0) final_offset = offset;
            break;
        }
        if ((b & 0xC0) == 0xC0) {
            if (offset + 1 >= pkt_len) return -1;
            int ptr = ((b & 0x3F) << 8) | pkt[offset + 1];
            if (final_offset < 0) final_offset = offset + 2;
            offset = ptr;
            if (++hops > 16) return -1;  /* loop guard */
            continue;
        }
        if (offset + 1 + b >= pkt_len) return -1;
        for (int i = 0; i < b; i++) {
            char c = (char)pkt[offset + 1 + i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (outpos < max - 2) out[outpos++] = c;
        }
        if (outpos < max - 2) out[outpos++] = '.';
        offset += 1 + b;
    }
    if (outpos < max) out[outpos] = 0;
    if (final_offset < 0) final_offset = orig_offset;
    return final_offset;
}

/* ------------------------------------------------------------------ */
/* Local IPv4 address discovery                                       */
/* ------------------------------------------------------------------ */

static unsigned long pick_ipv4_addr(void) {
    /* GetAdaptersAddresses to find the first non-loopback IPv4. */
    ULONG bufsize = 16 * 1024;
    IP_ADAPTER_ADDRESSES *adapters = (IP_ADAPTER_ADDRESSES*)malloc(bufsize);
    if (!adapters) return 0;
    DWORD ret = GetAdaptersAddresses(AF_INET,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        NULL, adapters, &bufsize);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        free(adapters);
        adapters = (IP_ADAPTER_ADDRESSES*)malloc(bufsize);
        if (!adapters) return 0;
        ret = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            NULL, adapters, &bufsize);
    }
    unsigned long addr = 0;
    if (ret == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES *a = adapters; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
                if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
                struct sockaddr_in *sin = (struct sockaddr_in*)u->Address.lpSockaddr;
                addr = sin->sin_addr.S_un.S_addr;
                break;
            }
            if (addr) break;
        }
    }
    free(adapters);
    return addr;
}

/* ------------------------------------------------------------------ */
/* Response builder                                                   */
/* ------------------------------------------------------------------ */

static int append_record(unsigned char *pkt, int *pos, int max,
                          const char *name, unsigned short type,
                          unsigned short klass, unsigned long ttl,
                          const unsigned char *rdata, unsigned short rdlen) {
    if (*pos + 256 > max) return -1;
    int n = encode_dns_name(name, pkt + *pos, max - *pos);
    if (n < 0) return -1;
    *pos += n;
    if (*pos + 10 + rdlen > max) return -1;
    /* TYPE / CLASS / TTL / RDLENGTH */
    pkt[(*pos)++] = (unsigned char)(type >> 8);
    pkt[(*pos)++] = (unsigned char)(type & 0xff);
    pkt[(*pos)++] = (unsigned char)(klass >> 8);
    pkt[(*pos)++] = (unsigned char)(klass & 0xff);
    pkt[(*pos)++] = (unsigned char)((ttl >> 24) & 0xff);
    pkt[(*pos)++] = (unsigned char)((ttl >> 16) & 0xff);
    pkt[(*pos)++] = (unsigned char)((ttl >> 8) & 0xff);
    pkt[(*pos)++] = (unsigned char)(ttl & 0xff);
    pkt[(*pos)++] = (unsigned char)(rdlen >> 8);
    pkt[(*pos)++] = (unsigned char)(rdlen & 0xff);
    if (rdlen > 0) memcpy(pkt + *pos, rdata, rdlen);
    *pos += rdlen;
    return 0;
}

/* Build a response packet with PTR + SRV + TXT + A records identifying
   our service. Returns total length, or -1 on overflow. */
static int build_full_response(unsigned char *pkt, int max) {
    if (max < 12) return -1;
    /* Header: ID=0, Flags=0x8400 (response, AA), QD=0, AN=4, NS=0, AR=0. */
    memset(pkt, 0, 12);
    pkt[2] = 0x84;
    pkt[3] = 0x00;
    pkt[7] = 4;  /* ANCOUNT = 4 */
    int pos = 12;

    /* PTR: _remote-hands._tcp.local. -> <instance>._remote-hands._tcp.local. */
    unsigned char rd_ptr[256];
    int n = encode_dns_name(g_disc.instance_name, rd_ptr, sizeof(rd_ptr));
    if (n < 0) return -1;
    if (append_record(pkt, &pos, max, g_disc.service_name,
                      DNS_TYPE_PTR, DNS_CLASS_IN, MDNS_TTL,
                      rd_ptr, (unsigned short)n) < 0) return -1;

    /* SRV: <instance> -> priority/weight/port/target */
    unsigned char rd_srv[300];
    int srv_pos = 0;
    rd_srv[srv_pos++] = 0; rd_srv[srv_pos++] = 0;            /* priority */
    rd_srv[srv_pos++] = 0; rd_srv[srv_pos++] = 0;            /* weight */
    rd_srv[srv_pos++] = (unsigned char)(g_disc.port >> 8);   /* port */
    rd_srv[srv_pos++] = (unsigned char)(g_disc.port & 0xff);
    int target_n = encode_dns_name(g_disc.host_dotted,
                                   rd_srv + srv_pos, (int)sizeof(rd_srv) - srv_pos);
    if (target_n < 0) return -1;
    srv_pos += target_n;
    if (append_record(pkt, &pos, max, g_disc.instance_name,
                      DNS_TYPE_SRV, DNS_CLASS_IN | DNS_FLUSH_BIT, MDNS_TTL,
                      rd_srv, (unsigned short)srv_pos) < 0) return -1;

    /* TXT: <instance> -> our pre-built TXT payload. */
    if (append_record(pkt, &pos, max, g_disc.instance_name,
                      DNS_TYPE_TXT, DNS_CLASS_IN | DNS_FLUSH_BIT, MDNS_TTL,
                      (const unsigned char*)g_disc.txt_payload,
                      (unsigned short)g_disc.txt_payload_len) < 0) return -1;

    /* A: <host>.local. -> ipv4 */
    unsigned char rd_a[4];
    memcpy(rd_a, &g_disc.ipv4_addr_n, 4);
    if (append_record(pkt, &pos, max, g_disc.host_dotted,
                      DNS_TYPE_A, DNS_CLASS_IN | DNS_FLUSH_BIT, MDNS_TTL,
                      rd_a, 4) < 0) return -1;

    return pos;
}

/* Build a TXT record body of length-prefixed key=value entries. */
static int build_txt(const char *os_name, char *out, int max) {
    int pos = 0;
    auto add = [&](const char *kv) -> bool {
        int klen = (int)strlen(kv);
        if (klen > 255) klen = 255;
        if (pos + 1 + klen > max) return false;
        out[pos++] = (char)klen;
        memcpy(out + pos, kv, klen);
        pos += klen;
        return true;
    };
    char buf[128];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "protocol=1");                  add(buf);
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "os=%s", os_name);              add(buf);
    return pos;
}

/* ------------------------------------------------------------------ */
/* Responder thread                                                    */
/* ------------------------------------------------------------------ */

static unsigned __stdcall responder_thread(void *arg) {
    (void)arg;
    unsigned char buf[1500];
    while (InterlockedAdd((LONG*)&g_disc.running, 0)) {
        struct sockaddr_in src;
        int src_len = sizeof(src);
        int n = recvfrom(g_disc.sock, (char*)buf, sizeof(buf), 0,
                         (struct sockaddr*)&src, &src_len);
        if (n < 12) continue;
        /* Only respond to queries (QR=0). */
        if (buf[2] & 0x80) continue;
        /* Walk question section; respond if any question matches one of
           our advertised names with a relevant qtype. */
        int qd = (buf[4] << 8) | buf[5];
        int pos = 12;
        bool match = false;
        for (int i = 0; i < qd && pos < n; i++) {
            char qname[256];
            int next = decode_dns_name(buf, n, pos, qname, sizeof(qname));
            if (next < 0 || next + 4 > n) break;
            unsigned short qtype = (unsigned short)((buf[next] << 8) | buf[next + 1]);
            pos = next + 4;
            /* Compare lowercase. service_name and instance_name are stored
               lowercase already. */
            char svc_lc[128], inst_lc[256], host_lc[80];
            strncpy_s(svc_lc,  sizeof(svc_lc),  g_disc.service_name,  _TRUNCATE);
            strncpy_s(inst_lc, sizeof(inst_lc), g_disc.instance_name, _TRUNCATE);
            strncpy_s(host_lc, sizeof(host_lc), g_disc.host_dotted,   _TRUNCATE);
            if (_stricmp(qname, svc_lc) == 0 &&
                (qtype == DNS_TYPE_PTR || qtype == DNS_TYPE_ANY)) {
                match = true; break;
            }
            if (_stricmp(qname, inst_lc) == 0 &&
                (qtype == DNS_TYPE_SRV || qtype == DNS_TYPE_TXT ||
                 qtype == DNS_TYPE_ANY)) {
                match = true; break;
            }
            if (_stricmp(qname, host_lc) == 0 &&
                (qtype == DNS_TYPE_A || qtype == DNS_TYPE_ANY)) {
                match = true; break;
            }
        }
        if (!match) continue;

        unsigned char resp[1500];
        int resp_len = build_full_response(resp, sizeof(resp));
        if (resp_len <= 0) continue;
        /* Multicast the response so other listeners on the LAN can cache
           our records too — standard mDNS responder behaviour. */
        struct sockaddr_in dest;
        memset(&dest, 0, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_port = htons(MDNS_PORT);
        inet_pton(AF_INET, MDNS_GROUP, &dest.sin_addr);
        sendto(g_disc.sock, (const char*)resp, resp_len, 0,
               (struct sockaddr*)&dest, sizeof(dest));
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

static void to_lower_inplace(char *s) {
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}

extern "C" int discovery_start(unsigned short service_port, const char *os_name) {
    if (g_started) return 1;

    /* Pull computer name; fall back to "remote-hands" if unavailable. */
    char host[64];
    DWORD hlen = sizeof(host);
    if (!GetComputerNameA(host, &hlen) || hlen == 0) {
        strcpy_s(host, sizeof(host), "remote-hands");
    }
    /* DNS labels can't have whitespace or dots in names — sanitize. */
    for (char *p = host; *p; p++) {
        if (*p == ' ' || *p == '.' || *p == '_') *p = '-';
    }
    strncpy_s(g_disc.host_label, sizeof(g_disc.host_label), host, _TRUNCATE);
    to_lower_inplace(g_disc.host_label);

    strcpy_s(g_disc.service_name, sizeof(g_disc.service_name),
             "_remote-hands._tcp.local.");
    _snprintf_s(g_disc.instance_name, sizeof(g_disc.instance_name), _TRUNCATE,
                "%s._remote-hands._tcp.local.", g_disc.host_label);
    _snprintf_s(g_disc.host_dotted, sizeof(g_disc.host_dotted), _TRUNCATE,
                "%s.local.", g_disc.host_label);

    g_disc.txt_payload_len = build_txt(os_name, g_disc.txt_payload,
                                        sizeof(g_disc.txt_payload));
    g_disc.port = service_port;
    g_disc.ipv4_addr_n = pick_ipv4_addr();
    if (g_disc.ipv4_addr_n == 0) {
        fprintf(stderr, "discovery: no IPv4 interface; not advertising\n");
        return 0;
    }

    /* Open a UDP socket bound to 0.0.0.0:5353 with SO_REUSEADDR so we
       coexist with anything else on the box. Join the mDNS multicast
       group on every interface (best-effort; first failure is logged
       and ignored). */
    g_disc.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_disc.sock == INVALID_SOCKET) return 0;
    BOOL reuse = TRUE;
    setsockopt(g_disc.sock, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&reuse, sizeof(reuse));
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(MDNS_PORT);
    if (bind(g_disc.sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) != 0) {
        fprintf(stderr, "discovery: bind 5353 failed (already in use?); "
                        "skipping advertisement\n");
        closesocket(g_disc.sock);
        g_disc.sock = INVALID_SOCKET;
        return 0;
    }
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    inet_pton(AF_INET, MDNS_GROUP, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt(g_disc.sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
               (const char*)&mreq, sizeof(mreq));

    InterlockedExchange((LONG*)&g_disc.running, 1);
    uintptr_t h = _beginthreadex(NULL, 0, responder_thread, NULL, 0, NULL);
    if (h == 0) {
        closesocket(g_disc.sock);
        g_disc.sock = INVALID_SOCKET;
        InterlockedExchange((LONG*)&g_disc.running, 0);
        return 0;
    }
    g_disc.thread = (HANDLE)h;
    g_started = 1;
    printf("discovery: advertising %s on port %u\n",
           g_disc.instance_name, (unsigned)service_port);
    fflush(stdout);
    return 1;
}

extern "C" void discovery_stop(void) {
    if (!g_started) return;
    InterlockedExchange((LONG*)&g_disc.running, 0);
    /* Close the socket so the blocked recvfrom returns. */
    if (g_disc.sock != INVALID_SOCKET) {
        closesocket(g_disc.sock);
        g_disc.sock = INVALID_SOCKET;
    }
    if (g_disc.thread) {
        WaitForSingleObject(g_disc.thread, 2000);
        CloseHandle(g_disc.thread);
        g_disc.thread = NULL;
    }
    g_started = 0;
}

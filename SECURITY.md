# Security

## Reporting a vulnerability

Please report security vulnerabilities privately via [GitHub Security Advisories](https://github.com/WilliamIsted/agent-remote-hands/security/advisories/new) rather than opening a public issue.

We aim to acknowledge reports within a few working days. There is no published timeline guarantee — this is a best-effort project.

## Threat model

The agent is designed to give an authenticated, trusted operator (today: anyone who can reach TCP/8765) full control over the host, including:

- Synthetic keyboard / mouse / clipboard input
- Process start / kill, registry read/write, file read/write
- Screen capture and UI Automation introspection
- Window management

**There is no built-in authentication today.** Access control is provided by the network. Treat the listening port the same way you'd treat unauthenticated WinRM or VNC: only expose it on networks you trust.

mDNS discovery (`-Discoverable` / `REMOTE_HANDS_DISCOVERABLE=1`) is opt-in for this reason. Advertising on an untrusted network is a footgun — it tells every device on the LAN that an unauthenticated control surface is available.

## Roadmap

The auth model evolves across milestones (see [`README.md`](README.md)):

- **v1.0** — Per-connection capability tiers (`observe` / `drive` / `power`) gated by an `ELEVATE` verb, plus a file-token check so a caller proves filesystem access to the host before elevating. Mitigates accidental destructive actions; does not authenticate the network connection itself.
- **v2.0** — Privilege-separated dispatcher. The dispatcher process holds privilege; per-connection workers run with tier-restricted tokens. OS-enforced separation: a compromised worker cannot escalate beyond its tier regardless of agent code paths.
- **v3.0** — SSPI authentication (Negotiate / NTLM / Kerberos). Per-connection workers spawn under the authenticated caller's Windows identity, so filesystem and registry ACLs match the caller, not the agent. WinRM-equivalent auth model.

## Known limitations (current builds)

- No transport encryption. Treat the wire as cleartext.
- No rate limiting or connection-abuse protection beyond the advertised `max_connections` cap.
- The agent runs in the user's interactive desktop session via Task Scheduler logon-task autostart; anything running in that session can interact with it.

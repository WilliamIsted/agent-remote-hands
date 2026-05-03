# Changelog

Per-release notes for Agent Remote Hands. Each entry pins a wire-protocol
version (see [`PROTOCOL.md`](PROTOCOL.md) §12) and the agent build version
(reported by `system.info.version`).

## Unreleased — v0.3.0 (Protocol 2.1)

Wire-breaking. No alias period — agents on this build reject the v2.0
tier and verb names. Pin to v0.2.0-rc.5 for the Protocol 2.0 surface.

- **CRUDX tier ladder.** `observe` / `drive` / `power` become a five-rung
  ladder: `read` < `create` < `update` < `delete` < `extra_risky`. Each
  verb's required tier is derived from its CRUDX letter on
  `Repos/Protocol/spec/verbs/<verb>.json`.
- **Clipboard verb rename.** `clipboard.read` → `clipboard.get`,
  `clipboard.write` → `clipboard.set`.
- **Directory namespace split.** Directory-only verbs leave the `file.*`
  namespace and become `directory.*`: `file.list` → `directory.list`,
  `file.mkdir` → `directory.create`. Polymorphic verbs (`file.delete`,
  `file.stat`, `file.exists`, `file.wait`, `file.rename`) stay in
  `file.*` since they tolerate both files and directories.
- **New directory primitives.** `directory.stat`, `directory.exists`,
  `directory.rename` (with `--overwrite` and `--cross-fs` flags),
  `directory.remove` (with `--recursive` for `rm -rf` semantics; reparse
  points are not traversed).
- **MCP wrapper schema lift.** Tools now load their `input_schema` from
  `Repos/Protocol/spec/verbs/*.json` via `mcp-server/spec_loader.py`.
  LLM-facing tool names use the wire-verb form (`screen.capture`, not
  `take_screenshot`); composite tools (`element.click`, `wait_for_*`)
  keep semantic names with namespace prefixes.
- **Tier-elevation tools.** `request_drive_access` / `request_power_access`
  become four per-rung tools: `request_create_access`,
  `request_update_access`, `request_delete_access`,
  `request_extra_risky_access`. Holding a higher rung subsumes everything
  below per the ladder.

Migration:
- Clients raising to `drive` should now raise to `update`.
- Clients raising to `power` should now raise to `extra_risky`.
- `clipboard.read` / `clipboard.write` callers update verb names.
- `file.list` callers move to `directory.list`; `file.mkdir` to
  `directory.create`.

## v0.2.0 (Protocol 2.0)

- First ratified release of the Protocol 2.0 spec.
- Three-tier model (`observe` / `drive` / `power`) gated by
  `connection.tier_raise`, file-token auth.
- Single-process agent.
- mcp-server bridge with hand-maintained tool registry.

For older history, see git log.

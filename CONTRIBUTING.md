# Contributing

Thanks for the interest. Issues and pull requests are welcome.

## Filing issues

Use the templates under [`.github/ISSUE_TEMPLATE/`](.github/ISSUE_TEMPLATE/):

- **Agent feedback** — friction or gaps surfaced while driving the agent in real LLM tasks.
- **Bug report** — something is broken (wrong return shape, crash, conformance regression).
- **Enhancement** — new verb, new MCP tool, new capability, ergonomic improvement.

Security vulnerabilities go through [GitHub Security Advisories](https://github.com/WilliamIsted/agent-remote-hands/security/advisories/new), not public issues. See [`SECURITY.md`](SECURITY.md).

## Labels

| Label | Meaning |
|---|---|
| `agent-feedback` | Surfaced by an LLM agent using the tool in real tasks |
| `agent-authored` | Issue body or PR text written by an LLM (provenance marker) |
| `high-priority` | Direct cost — UX hit, observability gap, blocked workflows |
| `low-priority` | Polish — only worth doing if cheap |
| `bug` / `enhancement` / `documentation` | Standard |

There is no `medium-priority` label — absence of a `high-priority` / `low-priority` label means medium.

## Milestones

| Milestone | Theme |
|---|---|
| `v1.0` | Stable protocol + per-connection tier system + agent-feedback fixes |
| `v2.0` | Privsep dispatcher (privileged dispatcher + tier-restricted workers) |
| `v3.0` | SSPI auth + caller impersonation |

Most agent-feedback issues live in v1.0. v2.0 and v3.0 are architectural increments that change the security model.

## Workflow

1. Branch off `main`. PRs target `main`.
2. Commits: short, bullet-pointed, present-tense. Mirror existing log style.
3. Run the conformance suite against a healthy agent before submitting:
   ```bash
   python tests/conformance/run.py <host> 8765
   ```
4. Open the PR using the template at [`.github/pull_request_template.md`](.github/pull_request_template.md).

Don't merge the `benchmark` branch into `main` — it's a fixture branch holding tool-benchmark and comparison rigs, not product.

## Wire-protocol changes

Any change to the wire protocol must touch three places:

1. [`PROTOCOL.md`](PROTOCOL.md) — verb name, args, success / error shape, capability flag.
2. The agent source — `agents/windows-modern/` and `agents/windows-nt/` where applicable. New verbs advertise their capability flag in `INFO` / `CAPS` only when the underlying API is available.
3. A conformance test under `tests/conformance/`. Tests gate on the capability flag, so agents that don't implement the verb get the test skipped, not failed.

PRs missing any of the three are incomplete.

## MCP tool wrappers

If the wire change should be exposed as a named MCP tool:

1. Implement the handler in `mcp-server/tools.py`.
2. Gate registration on the wire capability flag — the tool must not be advertised if the agent doesn't speak the underlying verb.
3. Use semantic naming (`click_element` over `click_at_xy`).
4. Annotate with `destructiveHint` / `readOnlyHint` to nudge the model toward safe choices.
5. The description string matters — Claude reads it. Be specific about what the tool does and when it's the right choice over alternatives.

## Building

See [`CLAUDE.md`](CLAUDE.md) for build commands and the Vagrant dev fixture under `examples/vagrant/`.

## License

By contributing you agree your contributions are licensed under the project's license (TBD).

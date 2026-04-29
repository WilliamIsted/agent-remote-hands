# Scoop manifest

Reference manifest for the [Scoop](https://scoop.sh/) Windows package manager.
The actual published manifest lives in
[`WilliamIsted/scoop-bucket`](https://github.com/WilliamIsted/scoop-bucket) at
`bucket/agent-remote-hands.json` — this file is the source-of-truth template
that gets copied across when a new release lands.

## Updating after a new release

The `.github/workflows/release.yml` workflow attaches a SHA256SUMS file to
each GitHub Release. To roll the bucket forward:

1. Pick up the new SHA256 from the release's `SHA256SUMS` asset.
2. In the bucket repo, edit `bucket/agent-remote-hands.json` — bump
   `version`, update the `url` (the `v$version` substitutions resolve at
   `scoop install` time but the explicit version tag and matching hash
   need to be fresh per release), and paste the new hash.
3. Commit + push. `scoop update agent-remote-hands` on a user's machine
   picks up the new manifest from the bucket on the next refresh.

Or use Scoop's autoupdate tooling — `scoop checkver -u agent-remote-hands`
in the bucket repo bumps the version + hash automatically. A scheduled
GitHub Action in the bucket repo can run this on a cron and open a PR.

## End-user install

```powershell
# One-time bucket add:
scoop bucket add isted https://github.com/WilliamIsted/scoop-bucket

# Install:
scoop install agent-remote-hands

# Complete the system-level setup (firewall + Task Scheduler):
agent-remote-hands-setup -Discoverable
```

The `bin` field exposes both the agent (`remote-hands.exe`) and a
shim for the installer script (`agent-remote-hands-setup`, alias for
`install-agent.ps1`).

## Defender note

If Scoop's download fails because Defender quarantines `remote-hands.exe`,
add a Defender exclusion for `~/scoop` first:

```powershell
Add-MpPreference -ExclusionPath "$env:USERPROFILE\scoop"
```

This is common practice for Scoop users — false positives are frequent
across the ecosystem. The setup script adds an additional exclusion for
the install destination (`%ProgramFiles%\AgentRemoteHands`) as its first
step.

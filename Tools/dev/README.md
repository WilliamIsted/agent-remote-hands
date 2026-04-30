# Tools/dev — developer-only helpers

Scripts and recipes for working *on* the agent rather than deploying it. Not part of the production install path; not shipped in the release zip.

## `start-stable-agent.ps1` — dev-against-stable dogfooding

Launches a known-good agent build from `%USERPROFILE%\AgentRemoteHands-stable\` on port `18765` (default), so a Claude Code session in this repo can use the agent via the MCP bridge while you continue editing and rebuilding the dev binary in `agents/windows-modern/build/Release/` without disturbing the dogfooding loop.

The architectural shape:

```
   This dev Claude Code session                    Stable agent
   (edits source, runs builds,            <---->   (port 18765, locked
    pure dev mode usually)                          binary, separate token)
                |
                v
   .mcp.json (gitignored, repo root) ----> mcp-server/server.py ---> stable agent

   meanwhile, ad-hoc dev test runs:
   build\Release\remote-hands.exe --port 28765 --token-path C:\temp\dev-token
   (different port, different token, no MCP wiring; conformance suite hits it)
```

Two ports, two tokens, two roles. The dev session never has to wonder whether the agent it's hitting via MCP includes the half-finished change you're staring at in the editor.

### Initial setup

```powershell
# 1. Make sure agents/windows-modern/build/Release/remote-hands.exe is the
#    build you want to lock as "stable for dogfooding". Conformance should
#    pass against it before promoting.
python tests/conformance/run.py 127.0.0.1 18765

# 2. Stage the binary + companion docs into the stable dir.
$stable = Join-Path $env:USERPROFILE 'AgentRemoteHands-stable'
New-Item -ItemType Directory -Force -Path $stable | Out-Null
$repo = (Resolve-Path '.').Path
Copy-Item "$repo\agents\windows-modern\build\Release\remote-hands.exe" $stable -Force
Copy-Item "$repo\agents\windows-modern\build\Release\remote-hands.pdb" $stable -Force
Copy-Item "$repo\LLM-OPERATORS.md" $stable -Force
Copy-Item "$repo\PROTOCOL.md"      $stable -Force
Copy-Item "$repo\README.md"        $stable -Force
Copy-Item "$repo\tests\conformance\wire.py" $stable -Force

# 3. (Optional, recommended) Add the stable dir to Defender's exclusion list.
#    Requires elevated PowerShell.
Add-MpPreference -ExclusionPath $stable

# 4. Drop a .mcp.json at the repo root (this directory itself; it's
#    gitignored). See the snippet below.

# 5. Restart Claude Code. /mcp should now show the agent-remote-hands server
#    with 27 tools available after the bridge connects.
```

### `.mcp.json` snippet

Save as `.mcp.json` at the repo root. The repo's `.gitignore` already excludes it.

```json
{
  "mcpServers": {
    "agent-remote-hands": {
      "command": "python",
      "args": ["mcp-server/server.py"],
      "env": {
        "REMOTE_HANDS_HOST":       "127.0.0.1",
        "REMOTE_HANDS_PORT":       "18765",
        "REMOTE_HANDS_TOKEN_PATH": "C:\\Users\\Admin\\AgentRemoteHands-stable\\token"
      }
    }
  }
}
```

(Substitute `Admin` with your username, or use `$env:USERPROFILE` interpolation if you generate the file from a script.)

### Daily use

```powershell
# Start the stable agent (run in any terminal; agent stays in foreground).
.\Tools\dev\start-stable-agent.ps1

# In a separate Claude Code session, the bridge connects on demand. Try:
#   "call agent_info"
# and confirm it returns the stable build's system.info.

# When you ship a new RC and want the dogfooding session to use it:
.\Tools\dev\start-stable-agent.ps1 -RefreshFromBuild
# Copies the current build/Release exe over the stable copy first.
```

### Why not just install via `Tools/install-agent.ps1`?

You can. The production install gives you a Task-Scheduler-backed agent on port 8765 with admin install + firewall rules + `%ProgramData%` token. That's the right shape for production deployments and it works fine for dogfooding too.

`start-stable-agent.ps1` is for the case where you don't want admin elevation and don't want the agent persisted across reboots — useful when you're frequently bouncing between binaries (e.g., comparing rc.7 vs rc.8 behaviour by swapping the stable copy).

If you have the production install going, you can point `.mcp.json` at port 8765 instead of 18765 and skip this script entirely.

### Refreshing the stable copy

The stable copy is deliberately *not* auto-updated when you rebuild. That's the point — your dev session keeps editing source while the dogfooding session keeps hitting a frozen binary. To promote a new build:

```powershell
# Stop the running stable agent (Ctrl+C in its terminal).
# Then refresh + restart:
.\Tools\dev\start-stable-agent.ps1 -RefreshFromBuild
```

Confirm the SHA in the startup banner matches what you expect.

### Why this isn't shipped in the release zip

This is dev-only infrastructure. End users / operators get the binary + docs via the standard release zip + Scoop manifest. They don't need a "stable copy of stable" pattern.

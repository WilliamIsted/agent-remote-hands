#   Copyright 2026 William Isted and contributors
#
#   Licensed under the Apache License, Version 2.0 (the "License");
#   you may not use this file except in compliance with the License.
#   You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
#   Unless required by applicable law or agreed to in writing, software
#   distributed under the License is distributed on an "AS IS" BASIS,
#   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#   See the License for the specific language governing permissions and
#   limitations under the License.

# start-stable-agent.ps1 — launch the dev-against-stable agent.
#
# Starts a previously-staged "stable" agent build from
# %USERPROFILE%\AgentRemoteHands-stable\ on a chosen port. Use this when you
# want a Claude Code (or other MCP client) session in this dev repo to
# dogfood the agent — the bridge in mcp-server/ talks to this stable copy,
# while you continue editing/rebuilding the dev binary in
# agents/windows-modern/build/Release/ without disturbing the dogfooding
# session.
#
# This is NOT a production install. It does not touch:
#   - Program Files
#   - Task Scheduler
#   - Windows Firewall
#   - The %ProgramData% token path
#
# Production install lives in Tools/install-agent.ps1; that path is
# admin-elevated and persistent. This script is user-mode and ad-hoc.
#
# Usage:
#   .\Tools\dev\start-stable-agent.ps1                 # default port 18765
#   .\Tools\dev\start-stable-agent.ps1 -Port 8765      # match production port
#   .\Tools\dev\start-stable-agent.ps1 -RefreshFromBuild  # copy current
#                                                         # build/Release exe
#                                                         # over the stable
#                                                         # snapshot first

[CmdletBinding()]
param(
    [int]$Port = 18765,
    [string]$StableDir = (Join-Path $env:USERPROFILE 'AgentRemoteHands-stable'),
    [switch]$RefreshFromBuild,
    [switch]$AddDefenderExclusion
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $StableDir)) {
    throw "Stable agent dir not found at $StableDir. Set up via Tools/dev/README.md or pass -StableDir <path>."
}

if ($RefreshFromBuild) {
    $repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $build    = Join-Path $repoRoot 'agents\windows-modern\build\Release\remote-hands.exe'
    if (-not (Test-Path $build)) {
        throw "Build artefact not at $build. Run cmake --build first, or drop -RefreshFromBuild."
    }
    Write-Host "Refreshing $StableDir\remote-hands.exe from $build"
    Copy-Item $build (Join-Path $StableDir 'remote-hands.exe') -Force
    $pdb = Join-Path $repoRoot 'agents\windows-modern\build\Release\remote-hands.pdb'
    if (Test-Path $pdb) {
        Copy-Item $pdb (Join-Path $StableDir 'remote-hands.pdb') -Force
    }
}

if ($AddDefenderExclusion) {
    # Per-process exclusion only (file scans). Doesn't disable behaviour
    # monitoring. Requires admin — will fail gracefully if run unelevated.
    try {
        Add-MpPreference -ExclusionPath $StableDir -ErrorAction Stop
        Write-Host "Defender path exclusion added: $StableDir"
    } catch {
        Write-Warning "Couldn't add Defender exclusion (need admin): $_"
        Write-Warning "Continuing without — agent may be flagged by realtime scan."
    }
}

$exe       = Join-Path $StableDir 'remote-hands.exe'
$tokenPath = Join-Path $StableDir 'token'

if (-not (Test-Path $exe)) {
    throw "Agent binary not found at $exe."
}

Write-Host ""
Write-Host "=== Dev-against-stable agent ==="
Write-Host "  binary       : $exe"
Write-Host "  token path   : $tokenPath  (auto-rotated by the agent on launch)"
Write-Host "  port         : $Port"
Write-Host "  SHA256       : $((Get-FileHash $exe -Algorithm SHA256).Hash)"
Write-Host ""
Write-Host "  agent will run in this terminal; Ctrl+C to stop."
Write-Host "  point .mcp.json at 127.0.0.1:$Port and the token file above."
Write-Host ""

& $exe --port $Port --token-path $tokenPath

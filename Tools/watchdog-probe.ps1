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

<#
.SYNOPSIS
    Liveness probe for the AgentRemoteHands agent.

.DESCRIPTION
    Runs as SYSTEM via the AgentRemoteHandsWatchdog scheduled task every 60s.
    Probes the agent with the loopback PING/PONG sub-MCP fast-path: TCP
    connect to 127.0.0.1:8765, write "PING\n", read exactly "PONG\n", close.
    The agent's accept loop intercepts these 5 bytes on loopback and
    replies without engaging framing OR logging — see server.cpp.

    Catches the most common wedge modes:
      * Accept loop wedged          -> TCP connect times out
      * PING-handler thread starved -> read times out before PONG arrives
    The fast-path is behaviourally equivalent to invoking the system.ping
    verb (Tier::Read, empty body); a remote client that can't speak the
    fast-path would call the verb instead.

    Logs every run (success or kill) to
    %ProgramData%\AgentRemoteHands\watchdog.log so the operator can tail
    the agent's liveness history alongside any forensic capture timeline.

    Exit codes:
       0 — agent responsive (no action taken)
       1 — agent killed (probe failure)
       2 — internal error (logged; no kill)
#>

[CmdletBinding()]
param(
    [string]$AgentHost = '127.0.0.1',
    [int]$AgentPort    = 8765,
    [int]$ConnectTimeoutMs = 2000,
    [int]$PingTimeoutMs    = 2000,
    [string]$ProcessName   = 'rha-win.modern.x64'
)

$ErrorActionPreference = 'Stop'

$LogDir  = Join-Path $env:ProgramData 'AgentRemoteHands'
$LogFile = Join-Path $LogDir 'watchdog.log'
New-Item -ItemType Directory -Force -Path $LogDir -EA SilentlyContinue | Out-Null

function Write-WatchdogLog {
    param([string]$Level, [string]$Message)
    $line = '{0}  {1}  {2}' -f `
        (Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff'), $Level, $Message
    try { Add-Content -Path $LogFile -Value $line -EA Stop }
    catch { }   # never let logging failure escalate
}

function Kill-Agent {
    param([string]$Reason)
    Write-WatchdogLog 'KILL' $Reason
    Get-Process -Name $ProcessName -EA SilentlyContinue |
        Stop-Process -Force -EA SilentlyContinue
}

# ---------- Probe: PING/PONG sub-MCP fast-path ----------
$tcp = $null
$stream = $null
try {
    # ---- TCP connect ----
    $tcp = New-Object System.Net.Sockets.TcpClient
    $async = $tcp.BeginConnect($AgentHost, $AgentPort, $null, $null)
    if (-not $async.AsyncWaitHandle.WaitOne($ConnectTimeoutMs, $false)) {
        $tcp.Close()
        Kill-Agent "TCP connect to ${AgentHost}:${AgentPort} timed out after ${ConnectTimeoutMs}ms"
        exit 1
    }
    try {
        $tcp.EndConnect($async)
    } catch {
        Kill-Agent ("TCP connect to {0}:{1} refused/failed: {2}" -f `
            $AgentHost, $AgentPort, $_.Exception.Message)
        exit 1
    }

    # ---- PING/PONG roundtrip ----
    # Send exactly "PING\n" (5 bytes), read exactly "PONG\n" (5 bytes).
    # The agent's accept loop intercepts these bytes on loopback and
    # replies without engaging MCP framing or emitting "Accepted
    # connection from..." to the agent log.
    $stream = $tcp.GetStream()
    $stream.WriteTimeout = $PingTimeoutMs
    $stream.ReadTimeout  = $PingTimeoutMs
    $ping = [byte[]]@(0x50, 0x49, 0x4E, 0x47, 0x0A)   # "PING\n"
    $stream.Write($ping, 0, 5)
    $stream.Flush()

    $resp = New-Object byte[] 5
    $off = 0
    while ($off -lt 5) {
        $read = $stream.Read($resp, $off, 5 - $off)
        if ($read -le 0) {
            Kill-Agent "PING/PONG: connection closed after $off bytes"
            exit 1
        }
        $off += $read
    }
    if (-not ($resp[0] -eq 0x50 -and $resp[1] -eq 0x4F -and
              $resp[2] -eq 0x4E -and $resp[3] -eq 0x47 -and
              $resp[4] -eq 0x0A)) {
        $hex = ($resp | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
        Kill-Agent "PING/PONG: expected 'PONG\n', got bytes $hex"
        exit 1
    }

    Write-WatchdogLog 'OK' "agent ping/pong roundtrip succeeded"
    exit 0
} catch [System.IO.IOException] {
    Kill-Agent ("PING/PONG read/write timed out: " + $_.Exception.Message)
    exit 1
} catch {
    Write-WatchdogLog 'ERROR' ("Probe setup failed: " + $_.Exception.Message)
    exit 2
} finally {
    if ($stream) { $stream.Close() }
    if ($tcp)    { $tcp.Close() }
}

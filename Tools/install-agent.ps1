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
    Install / uninstall the Agent Remote Hands wire-protocol agent.

.DESCRIPTION
    Copies remote-hands.exe to %ProgramFiles%\AgentRemoteHands, adds
    binary-scoped Windows Firewall rules, and registers a Task Scheduler
    logon-task with restart-on-failure. Run from an elevated PowerShell.

    Installation logic lives in this script (rather than in the agent
    binary) because Microsoft Defender's machine-learning heuristic
    flags self-installing binaries as Program:Win32/Contebrew.A!ml. The
    binary itself is purely a wire-protocol server.

.PARAMETER Source
    Path to remote-hands.exe to install. Defaults to the binary next to
    this script, falling back to the repo build path
    (../agents/windows-modern/build/Release/remote-hands.exe).

.PARAMETER Port
    TCP port the agent should listen on. Default 8765.

.PARAMETER Discoverable
    Pass through `--discoverable` so the agent advertises via mDNS.
    Adds an additional firewall rule for UDP 5353 inbound.

.PARAMETER Uninstall
    Reverse the install: unregister the task, remove firewall rules,
    delete the installed binary.

.EXAMPLE
    .\install-agent.ps1 -Discoverable

.EXAMPLE
    .\install-agent.ps1 -Port 8766

.EXAMPLE
    .\install-agent.ps1 -Uninstall
#>

[CmdletBinding()]
param(
    [string]$Source,
    [int]$Port = 8765,
    [switch]$Discoverable,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

# Constants
$InstallDir   = Join-Path $env:ProgramFiles 'AgentRemoteHands'
$BinaryName   = 'remote-hands.exe'
$BinaryPath   = Join-Path $InstallDir $BinaryName
$TaskName     = 'AgentRemoteHands'
$RuleName     = 'Agent Remote Hands'
# BUILTIN\Users SID — task fires for any logged-on Users-group member,
# matching the prior C++ XML's <GroupId>S-1-5-32-545</GroupId>.
$UsersGroupSid = 'S-1-5-32-545'

function Assert-Admin {
    $current = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($current)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'This script requires Administrator privileges. Right-click PowerShell and "Run as Administrator", then re-run.'
    }
}

function Resolve-Source {
    if ($Source) {
        if (-not (Test-Path $Source)) {
            throw "Source binary not found at: $Source"
        }
        return (Resolve-Path $Source).Path
    }

    # Default search: next to script, then repo build path.
    $candidates = @(
        (Join-Path $PSScriptRoot $BinaryName),
        (Join-Path $PSScriptRoot "..\agents\windows-modern\build\Release\$BinaryName")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return (Resolve-Path $c).Path }
    }
    throw "remote-hands.exe not found next to this script or in agents\windows-modern\build\Release\. Pass -Source <path> explicitly."
}

function Install-Agent {
    Assert-Admin
    $src = Resolve-Source

    Write-Host "Installing remote-hands.exe to $InstallDir ..."
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    Copy-Item -Path $src -Destination $BinaryPath -Force
    Write-Host "  binary -> $BinaryPath"

    # Firewall rules. Both scoped to the binary so Defender's per-app
    # firewall layer is satisfied (port-only rules don't pre-authorise
    # the app — Defender still prompts on first launch). Profile=Any
    # covers Private + Public + Domain (host-only adapters classify as
    # Public by default).
    Write-Host "Configuring firewall ..."
    Get-NetFirewallRule -DisplayName $RuleName -ErrorAction SilentlyContinue |
        Remove-NetFirewallRule -ErrorAction SilentlyContinue

    New-NetFirewallRule `
        -DisplayName $RuleName `
        -Direction Inbound -Protocol TCP -LocalPort $Port `
        -Program $BinaryPath -Action Allow -Profile Any |
        Out-Null
    Write-Host "  TCP $Port inbound, scoped to binary"

    if ($Discoverable) {
        New-NetFirewallRule `
            -DisplayName $RuleName `
            -Direction Inbound -Protocol UDP -LocalPort 5353 `
            -Program $BinaryPath -Action Allow -Profile Any |
            Out-Null
        Write-Host "  UDP 5353 (mDNS) inbound, scoped to binary"
    }

    # Task Scheduler logon-task with restart-on-failure. Equivalent to
    # the prior C++-generated XML (LogonTrigger, BUILTIN\Users principal,
    # HighestAvailable elevation, restart 3x at 1 minute intervals).
    Write-Host "Registering scheduled task ..."
    $taskArgs = @()
    if ($Discoverable)   { $taskArgs += '--discoverable' }
    if ($Port -ne 8765)  { $taskArgs += '--port'; $taskArgs += "$Port" }

    $action = New-ScheduledTaskAction -Execute $BinaryPath `
        -Argument ($taskArgs -join ' ')
    $trigger = New-ScheduledTaskTrigger -AtLogOn
    $principal = New-ScheduledTaskPrincipal `
        -GroupId $UsersGroupSid -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet `
        -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries `
        -StartWhenAvailable `
        -RestartCount 3 `
        -RestartInterval (New-TimeSpan -Minutes 1) `
        -ExecutionTimeLimit (New-TimeSpan)   # 0 = no time limit

    # Force replaces an existing task with the same name.
    Register-ScheduledTask -TaskName $TaskName `
        -Action $action -Trigger $trigger `
        -Principal $principal -Settings $settings `
        -Force | Out-Null
    Write-Host "  '$TaskName' registered (restart 3x at 1 minute on non-zero exit)"

    Write-Host ""
    Write-Host "Install complete."
    Write-Host "The agent will autostart on the next user logon."
    Write-Host "Run it now for this session:"
    if ($Discoverable) {
        Write-Host "  & '$BinaryPath' --discoverable"
    } else {
        Write-Host "  & '$BinaryPath'"
    }
}

function Uninstall-Agent {
    Assert-Admin

    Write-Host "Removing scheduled task ..."
    if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
        Write-Host "  '$TaskName' removed"
    } else {
        Write-Host "  '$TaskName' was not present"
    }

    Write-Host "Removing firewall rules ..."
    $rules = Get-NetFirewallRule -DisplayName $RuleName -ErrorAction SilentlyContinue
    if ($rules) {
        $rules | Remove-NetFirewallRule
        Write-Host "  '$RuleName' rule(s) removed ($($rules.Count) total)"
    } else {
        Write-Host "  no rules with display name '$RuleName' found"
    }

    Write-Host "Removing binary ..."
    if (Test-Path $BinaryPath) {
        Remove-Item -Path $BinaryPath -Force
        Write-Host "  removed $BinaryPath"
    } else {
        Write-Host "  $BinaryPath was not present"
    }
    if (Test-Path $InstallDir) {
        # only succeeds if empty — leaves user data alone
        Remove-Item -Path $InstallDir -Force -ErrorAction SilentlyContinue
    }

    Write-Host ""
    Write-Host "Uninstall complete."
    Write-Host "Note: the elevation token at %ProgramData%\AgentRemoteHands\token is left in place."
    Write-Host "Delete it manually if you want a full wipe."
}

try {
    if ($Uninstall) {
        Uninstall-Agent
    } else {
        Install-Agent
    }
} catch {
    Write-Host -ForegroundColor Red "ERROR: $_"
    exit 1
}

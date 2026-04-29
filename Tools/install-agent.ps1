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
    Adds a Microsoft Defender exclusion for the install directory, copies
    remote-hands.exe into it, adds binary-scoped Windows Firewall rules,
    and registers a Task Scheduler logon-task with restart-on-failure.
    Run from an elevated PowerShell.

    The Defender exclusion is the load-bearing step. Microsoft Defender's
    machine-learning heuristic flags any unsigned remote-control tool of
    this shape (synthetic input + screen capture + arbitrary file I/O +
    process kill + TCP listener) as Program:Win32/Contebrew.A!ml. Without
    a code-signing certificate the only mitigation is to deploy into an
    explicitly-excluded path so Defender doesn't scan it.

    Workflow on a fresh VM:

      1. Get this .ps1 onto the VM (PS scripts are not flagged the same way).
      2. Run from elevated PowerShell with -PrepareDefender FIRST so the
         install directory is excluded BEFORE the binary lands. Then download
         remote-hands.exe straight to the excluded path.
      3. Run again normally (no -PrepareDefender) to do the actual install.

    Or use -SourceUrl to fetch the binary directly into the excluded path
    in a single command — Invoke-WebRequest writes directly to the dest
    so the binary never touches a Defender-watched location.

.PARAMETER Source
    Local path to remote-hands.exe. Defaults to the binary next to this
    script, falling back to the repo build path
    (../agents/windows-modern/build/Release/remote-hands.exe).

.PARAMETER SourceUrl
    URL to fetch remote-hands.exe from instead of using a local file.
    Downloads directly into the (excluded) install dir, bypassing
    Defender's typical Downloads-folder scan.

.PARAMETER Port
    TCP port the agent should listen on. Default 8765.

.PARAMETER Discoverable
    Pass through `--discoverable` so the agent advertises via mDNS.
    Adds an additional firewall rule for UDP 5353 inbound.

.PARAMETER PrepareDefender
    Add the Defender exclusion ONLY. Use this on a fresh VM before
    downloading the binary so the destination path is already excluded
    when remote-hands.exe arrives.

.PARAMETER SkipDefenderExclusion
    Don't add a Defender exclusion. The default behaviour adds one for
    the install dir; pass this if you've already configured exclusions
    via Group Policy or you want Defender to keep scanning the binary
    (it will quarantine it).

.PARAMETER Uninstall
    Reverse the install: unregister the task, remove firewall rules,
    delete the installed binary, remove the Defender exclusion.

.EXAMPLE
    # First time on a VM — prepare Defender exclusion before downloading.
    .\install-agent.ps1 -PrepareDefender

.EXAMPLE
    # All-in-one: pull the binary from a URL into the excluded path.
    .\install-agent.ps1 -SourceUrl https://example/remote-hands.exe -Discoverable

.EXAMPLE
    # Local binary, full install.
    .\install-agent.ps1 -Discoverable

.EXAMPLE
    .\install-agent.ps1 -Uninstall
#>

[CmdletBinding()]
param(
    [string]$Source,
    [string]$SourceUrl,
    [int]$Port = 8765,
    [switch]$Discoverable,
    [switch]$PrepareDefender,
    [switch]$SkipDefenderExclusion,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

# Constants
$InstallDir    = Join-Path $env:ProgramFiles 'AgentRemoteHands'
$BinaryName    = 'remote-hands.exe'
$BinaryPath    = Join-Path $InstallDir $BinaryName
$TaskName      = 'AgentRemoteHands'
$RuleName      = 'Agent Remote Hands'
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

function Add-DefenderExclusion {
    # Idempotent — Add-MpPreference silently no-ops if the path is already
    # excluded. We also check Get-MpPreference so we can log a clear "already
    # there" message rather than a confusing silent success.
    $existing = (Get-MpPreference).ExclusionPath
    if ($existing -and ($existing -contains $InstallDir)) {
        Write-Host "  Defender exclusion already present for $InstallDir"
        return
    }
    try {
        Add-MpPreference -ExclusionPath $InstallDir
        Write-Host "  Defender exclusion added for $InstallDir"
    } catch {
        Write-Host -ForegroundColor Yellow "  WARNING: Could not add Defender exclusion: $_"
        Write-Host -ForegroundColor Yellow "           The binary may be quarantined on copy / first run."
    }
}

function Remove-DefenderExclusion {
    $existing = (Get-MpPreference).ExclusionPath
    if (-not $existing -or -not ($existing -contains $InstallDir)) {
        Write-Host "  no Defender exclusion to remove"
        return
    }
    try {
        Remove-MpPreference -ExclusionPath $InstallDir
        Write-Host "  Defender exclusion removed for $InstallDir"
    } catch {
        Write-Host -ForegroundColor Yellow "  WARNING: Could not remove Defender exclusion: $_"
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
    throw "remote-hands.exe not found next to this script or in agents\windows-modern\build\Release\. Pass -Source <path> or -SourceUrl <url>."
}

function Fetch-Source {
    # Pull the binary straight into the install dir so it never touches a
    # Defender-watched location like Downloads. The dir is excluded by this
    # point (Add-DefenderExclusion ran above), so Defender won't scan it on
    # arrival.
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    Write-Host "  fetching $SourceUrl ..."
    Invoke-WebRequest -Uri $SourceUrl -OutFile $BinaryPath -UseBasicParsing
    Write-Host "  fetched -> $BinaryPath"
    return $BinaryPath
}

function Prepare-Defender {
    Assert-Admin
    Write-Host "Adding Defender exclusion for $InstallDir ..."
    Add-DefenderExclusion
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    Write-Host ""
    Write-Host "Defender prep complete. You can now:"
    Write-Host "  1. Download remote-hands.exe directly to:  $InstallDir"
    Write-Host "  2. Run: .\install-agent.ps1$(if ($Discoverable) {' -Discoverable'})"
}

function Install-Agent {
    Assert-Admin

    # Defender exclusion FIRST so subsequent file ops aren't quarantined
    # mid-flight. The exclusion has to be in place before Copy-Item /
    # Invoke-WebRequest writes the binary to the install dir.
    if (-not $SkipDefenderExclusion) {
        Write-Host "Adding Defender exclusion ..."
        Add-DefenderExclusion
    } else {
        Write-Host "Skipping Defender exclusion (-SkipDefenderExclusion). Expect quarantine."
    }

    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

    # Acquire the binary — either from a local path or by URL fetch.
    $src = $null
    if ($SourceUrl) {
        $src = Fetch-Source
    } else {
        $src = Resolve-Source
        Write-Host "Installing remote-hands.exe to $InstallDir ..."
        if ((Resolve-Path $src).Path -ne $BinaryPath) {
            Copy-Item -Path $src -Destination $BinaryPath -Force
            Write-Host "  binary -> $BinaryPath"
        } else {
            Write-Host "  binary already at $BinaryPath"
        }
    }

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

    Write-Host "Removing Defender exclusion ..."
    Remove-DefenderExclusion

    Write-Host ""
    Write-Host "Uninstall complete."
    Write-Host "Note: the elevation token at %ProgramData%\AgentRemoteHands\token is left in place."
    Write-Host "Delete it manually if you want a full wipe."
}

try {
    if ($Uninstall) {
        Uninstall-Agent
    } elseif ($PrepareDefender) {
        Prepare-Defender
    } else {
        Install-Agent
    }
} catch {
    Write-Host -ForegroundColor Red "ERROR: $_"
    exit 1
}

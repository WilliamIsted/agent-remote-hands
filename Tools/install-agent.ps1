<#
.SYNOPSIS
  Installs Agent Remote Hands on a Windows machine - the windows-modern
  build. Run inside the target VM (or as part of provisioning).

.DESCRIPTION
  Places the binary in %ProgramFiles%\AgentRemoteHands, opens the firewall
  port, sets opt-in environment variables, registers the Task Scheduler
  logon task via the binary's own --install, and starts the agent in the
  current session so it's reachable immediately without waiting for next
  logon.

.PARAMETER LocalBinary
  Path to a remote-hands.exe already on the local filesystem (e.g. copied
  in by Vagrant's `file` provisioner).

.PARAMETER Url
  HTTP(S) URL to fetch remote-hands.exe from. Mutually exclusive with
  -LocalBinary.

.PARAMETER InstallPath
  Where the binary lands. Default: %ProgramFiles%\AgentRemoteHands.

.PARAMETER Port
  Listening port. Default: 8765. Persisted via REMOTE_HANDS_PORT if non-default.

.PARAMETER Discoverable
  Sets REMOTE_HANDS_DISCOVERABLE=1 so the agent broadcasts on the LAN
  via mDNS. Trust-network-only - see PROTOCOL.md.

.PARAMETER EnablePower
  Sets REMOTE_HANDS_POWER=1 so LOGOFF/REBOOT/SHUTDOWN verbs are exposed.

.EXAMPLE
  # From inside the VM, with the binary already copied in:
  .\install-agent.ps1 -LocalBinary C:\Windows\Temp\remote-hands.exe -Discoverable

.EXAMPLE
  # Download from a URL (corporate artifact server, GitHub Release, etc.):
  .\install-agent.ps1 -Url https://example.com/remote-hands.exe -Discoverable
#>
[CmdletBinding(DefaultParameterSetName='Local')]
param(
    [Parameter(ParameterSetName='Local', Mandatory)]
    [string]$LocalBinary,

    [Parameter(ParameterSetName='Remote', Mandatory)]
    [string]$Url,

    [string]$InstallPath = "$env:ProgramFiles\AgentRemoteHands",

    [int]$Port = 8765,

    [switch]$Discoverable,

    [switch]$EnablePower
)

$ErrorActionPreference = 'Stop'

# Require admin - Task Scheduler registration and firewall rule both need it.
$identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "install-agent.ps1 must run as Administrator."
    exit 1
}

# 1. Place the binary. Stop any running agent first - on a redeploy, the old
#    process holds an open handle on remote-hands.exe and the copy fails with
#    "file in use". Killing here is safe: this script is the canonical install
#    path, so the user is asking us to replace what's running.
New-Item -ItemType Directory -Force -Path $InstallPath | Out-Null
$dest = Join-Path $InstallPath 'remote-hands.exe'
$running = Get-Process -Name 'remote-hands' -ErrorAction SilentlyContinue
if ($running) {
    Write-Host "Stopping running agent (PIDs: $($running.Id -join ', '))"
    $running | Stop-Process -Force
    # Give Windows a moment to release the file lock.
    Start-Sleep -Milliseconds 500
}
if ($PSCmdlet.ParameterSetName -eq 'Remote') {
    Write-Host "Downloading $Url -> $dest"
    Invoke-WebRequest -Uri $Url -OutFile $dest -UseBasicParsing
} else {
    if (-not (Test-Path $LocalBinary)) {
        Write-Error "LocalBinary not found: $LocalBinary"
        exit 1
    }
    Write-Host "Copying $LocalBinary -> $dest"
    Copy-Item -Force $LocalBinary $dest
}

# 2. Firewall rules. Two are needed:
#    a) Port-based: opens TCP/$Port for the agent's listener.
#    b) Program-based: pre-authorises the binary path. Without this, Windows
#       Defender pops the "allow public/private networks" dialog the first
#       time the agent opens a listening socket - blocking remote access
#       until a user clicks Allow on the console. Adding a path-based Allow
#       rule short-circuits that prompt.
$ruleName = "Agent Remote Hands"
$existing = Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue
if (-not $existing) {
    Write-Host "Adding firewall rule: TCP/$Port inbound"
    New-NetFirewallRule -DisplayName $ruleName -Direction Inbound `
        -Protocol TCP -LocalPort $Port -Action Allow -Profile Any | Out-Null
} else {
    Write-Host "Firewall rule '$ruleName' already exists - skipping"
}

$progRuleName = "Agent Remote Hands (program)"
$progExisting = Get-NetFirewallRule -DisplayName $progRuleName -ErrorAction SilentlyContinue
if (-not $progExisting) {
    Write-Host "Adding firewall rule: program $dest inbound"
    New-NetFirewallRule -DisplayName $progRuleName -Direction Inbound `
        -Program $dest -Action Allow -Profile Any | Out-Null
} else {
    Write-Host "Firewall rule '$progRuleName' already exists - skipping"
}

# 3. Opt-in environment variables, persisted machine-wide so they survive
#    reboot and apply to the autostart task.
function Set-MachineEnv {
    param([string]$Name, [string]$Value)
    [Environment]::SetEnvironmentVariable($Name, $Value, 'Machine')
    Set-Item -Path "env:$Name" -Value $Value
}
if ($Discoverable) { Set-MachineEnv 'REMOTE_HANDS_DISCOVERABLE' '1' }
if ($EnablePower)  { Set-MachineEnv 'REMOTE_HANDS_POWER' '1' }
if ($Port -ne 8765) { Set-MachineEnv 'REMOTE_HANDS_PORT' "$Port" }

# 4. Register the Task Scheduler logon task. The agent's own --install does
#    this; we just invoke it. (See agents/windows-modern/agent.cpp do_install.)
Write-Host "Registering autostart task"
& $dest --install
if ($LASTEXITCODE -ne 0) {
    Write-Warning "remote-hands.exe --install exited with $LASTEXITCODE"
}

# 5. Start the agent. Two cases:
#
#    a) Interactive install (user is at the desktop running this script):
#       Start-Process puts the agent in the current session. Fine for
#       desktop-on-the-screen use because that IS session 1.
#
#    b) Provisioning install (Vagrant / Ansible / WinRM run this remotely):
#       The current session is session 0 (the WinRM service session), not
#       the desktop. An agent launched there can EXEC processes but they
#       won't surface UI, WINLIST returns empty, MPOS errors out - useless
#       for GUI automation. We need the agent running inside the logged-on
#       desktop session (session 1+).
#
#    The Task Scheduler task we just registered runs as the configured user
#    in their interactive session. Starting it via Start-ScheduledTask
#    triggers it on-demand without waiting for next logon, and lands the
#    agent in the right session regardless of where this script ran from.
Write-Host "Starting agent via scheduled task (lands in user desktop session)"
try {
    Start-ScheduledTask -TaskName 'RemoteHands'
    # Give it a beat to bind the port.
    Start-Sleep -Seconds 2
    $proc = Get-Process remote-hands -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $proc) {
        Write-Warning "scheduled task did not produce a remote-hands process; falling back to Start-Process (session may be wrong)"
        $proc = Start-Process -FilePath $dest -PassThru -WindowStyle Hidden
    }
} catch {
    Write-Warning "Start-ScheduledTask failed ($_); falling back to Start-Process"
    $proc = Start-Process -FilePath $dest -PassThru -WindowStyle Hidden
}

Write-Host ""
Write-Host "==============================================="
Write-Host "  Agent Remote Hands installed and running."
Write-Host "  Binary:   $dest"
Write-Host "  PID:      $($proc.Id)"
Write-Host "  Port:     $Port"
if ($Discoverable) {
    Write-Host "  mDNS:     advertising as $($env:COMPUTERNAME)._remote-hands._tcp.local."
}
if ($EnablePower) {
    Write-Host "  Power:    LOGOFF/REBOOT/SHUTDOWN verbs enabled"
}
Write-Host "  Autostart: registered as Task Scheduler logon task"
Write-Host "==============================================="
Write-Host ""
Write-Host "From the controller machine, find the agent:"
Write-Host "  python client/hostctl-discover"
Write-Host "Or test directly:"
Write-Host "  REMOTE_HANDS_HOST=<this-vm-ip> python client/hostctl ping"

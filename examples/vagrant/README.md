# Vagrant example

End-to-end dev fixture: spins up a Windows 11 VM with the Agent Remote Hands binary running, the firewall opened, and mDNS advertising enabled. Doubles as a smoke-test for the install path and as a starting template for users who want to provision agents into their own Vagrant-managed VMs.

## Prerequisites

- **Vagrant** ≥ 2.3 (`brew install vagrant` / `winget install Hashicorp.Vagrant` / [download](https://www.vagrantup.com/downloads))
- **A hypervisor**, one of:
  - VirtualBox 7+ (cross-platform, free)
  - Hyper-V (Windows host only — pass `--provider=hyperv`)
  - VMware Workstation/Fusion (paid; with `vagrant-vmware-desktop` plugin)
  - Parallels (Mac host; with `vagrant-parallels` plugin)
- **A built `remote-hands.exe`** at `agents/windows-modern/build/Release/remote-hands.exe`. Build it first:
  ```bash
  cd agents/windows-modern
  cmake -B build -A x64
  cmake --build build --config Release
  ```

## Usage

```bash
cd examples/vagrant
vagrant up
```

First run takes ~5–10 minutes — Vagrant downloads the Win11 base box (~6 GB), boots it, copies the agent in, and runs the installer. Subsequent runs reuse the cached box.

After it finishes, the VM is up with the agent running, listening on port 8765 (forwarded to `127.0.0.1:8765` on the host) and advertising itself on the LAN via mDNS.

## What you can do next

**Smoke test from the host:**
```bash
REMOTE_HANDS_HOST=127.0.0.1 python ../../client/hostctl ping
REMOTE_HANDS_HOST=127.0.0.1 python ../../client/hostctl shot-png /tmp/screen.png
```

**Find advertised agents on your LAN:**
```bash
python ../../client/hostctl-discover
```

**Wire into Claude Code:** add to `~/.claude/claude_desktop_config.json`:
```json
{
  "mcpServers": {
    "agent-remote-hands": {
      "command": "python",
      "args": ["/abs/path/to/repo/mcp-server/server.py"],
      "env": { "REMOTE_HANDS_HOST": "127.0.0.1" }
    }
  }
}
```

**Re-deploy after rebuilding the agent** (no need to destroy the VM):
```bash
cd ../../agents/windows-modern
cmake --build build --config Release
cd -
vagrant provision     # re-runs the file copy + install script
```

**Tear it all down:**
```bash
vagrant destroy
```

## Customising

The Vagrantfile is plain Ruby — edit it to:

- Switch base box (`gusztavvargadr/windows-10`, `gusztavvargadr/windows-server`, your own custom box)
- Add `vb.gui = true` (or `hv.vmname` etc.) to make the VM window visible — useful when watching what an AI agent is doing in real time
- Change resource sizing (memory, CPUs)
- Add the `-EnablePower` flag to the install script's args if you want `LOGOFF`/`REBOOT`/`SHUTDOWN` available
- Provision additional software (browsers, dev tools) before or after the agent install

## Troubleshooting

**"agent binary not found"** — build it first (see Prerequisites). The Vagrantfile fails fast if the binary isn't present.

**Build fails with `mt.exe` "Failed to load and parse the manifest"** — `manifest.xml` cannot contain `<trustInfo>` because `CMakeLists.txt` already passes `/MANIFESTUAC` to the linker, which synthesises one. Two `<trustInfo>` blocks in the merged manifest crash mt.exe. The included `manifest.xml` doesn't have the duplicate; if you've customised it, ensure no `<trustInfo>` sneaks back in. See `agents/windows-modern/BUILD.md`.

**`vagrant up` fails with `vmrun ... start ... nogui` "Unknown error"** — the VMware AuthD service (`VMAuthdService`) is required for vmrun's headless mode and is shipped disabled on many VMware Workstation installs. Two fixes:

1. Use the GUI mode the Vagrantfile already defaults to (`vmw.gui = true`). Works without admin; opens a VMware window per VM. Don't close that window while provisioning is in flight — the WinRM channel goes with it.
2. Enable the service properly (one-time, requires admin):
   ```powershell
   Start-Service VMAuthdService
   Set-Service VMAuthdService -StartupType Automatic
   ```
   Then set `vmw.gui = false` in the Vagrantfile for true headless operation.

**Provision step fails with "Access denied" on Task Scheduler** — the install script requires admin. Vagrant provisions with `privileged: true` by default for `winrm`-based boxes, so this should already be the case; if you've customised the Vagrantfile and disabled it, re-enable.

**Win11 base box won't boot / takes forever** — first boot includes the full sysprep dance. If it's been more than 15 minutes, `vagrant halt` then `vagrant up` again. The base box is sometimes flaky on the first boot after download.

**Hyper-V coexistence errors** — VMware Workstation 17.5+ runs alongside Hyper-V via WHPX automatically, no extra config needed. If your VMware is older than 17.5 and Hyper-V is enabled (WSL2 pulls it in), VMware can't acquire VT-x and VM start fails. Either upgrade VMware, disable Hyper-V (`bcdedit /set hypervisorlaunchtype off` + reboot, breaks WSL2), or pivot to the Hyper-V provider: `vagrant up --provider=hyperv` from an elevated PowerShell.

**mDNS not finding the VM** — the `forwarded_port` config on `127.0.0.1` is enough for host-side connections, but mDNS multicast doesn't always cross the NAT boundary back to the host. To advertise on your LAN, add a bridged interface to the Vagrantfile:
```ruby
config.vm.network "public_network"
```
The agent will be reachable directly at the bridged IP and `hostctl-discover` will find it.

## See also

- `Tools/install-agent.ps1` — the script Vagrant invokes; same script works outside Vagrant for any Windows machine
- `client/hostctl-discover` — host-side LAN scanner
- `mcp-server/README.md` — integrating with Claude Code

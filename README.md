# vm_agent — Win XP build-VM control agent

A tiny TCP server for the Windows XP build VM. Lets the host (your Mac) drive
the VM without VNC's per-frame overhead: synthesise mouse/keyboard events,
capture screenshots, run processes, and read/write files directly over a single
TCP connection.

## Files

- `agent.c` — single-file C source, builds with VC6 + Win32 SDK only
- `build.bat` — one-line VC6 build (run inside the VM)
- `vmctl` — host-side Python wrapper for the protocol
- `README.md` — this file

## Setup

### Inside the VM (one-time)

1. Copy `agent.c` and `build.bat` to a folder, e.g. `C:\vm_agent\`.
2. From a normal `cmd.exe` (no need for the VC6 prompt; `build.bat` calls
   `vcvars32.bat`):
   ```
   cd C:\vm_agent
   build.bat
   ```
3. Run `vm_agent.exe` (double-click or `start vm_agent.exe`). It opens a
   console window showing connections and commands.
   - Default port: 8765. Override: `vm_agent.exe 9999` or set `VM_AGENT_PORT`.

To auto-start at login, drop a shortcut to `vm_agent.exe` in
`%USERPROFILE%\Start Menu\Programs\Startup\`.

### On the Mac (one-time)

```bash
chmod +x tools/vm_agent/vmctl
ln -s "$PWD/tools/vm_agent/vmctl" /usr/local/bin/vmctl   # or add to PATH
export VM_HOST=192.168.x.x   # the VM's host-only network IP
# optional: export VM_PORT=8765
```

To find the VM's host-only IP, in the VM run `ipconfig` and look for the
adapter on the host-only subnet (often `192.168.56.x` or similar).

## Usage

```bash
vmctl ping                              # sanity check connection
vmctl screen                            # "OK 1024 768"
vmctl shot-png /tmp/vm.png              # screenshot, view with the Read tool
vmctl move 100 200 ; vmctl click left   # click somewhere
vmctl key alt-F4                        # close active window
vmctl keys "hello world"                # type literal text
vmctl drag 500 400 left                 # drag from current pos to (500,400)
vmctl wheel -120                        # mouse wheel one notch down

# Real workhorse — drive the build entirely without GUI:
vmctl exec "xcopy /E /Y X:\\gog_src\\Sourcecode für SB C:\\oat_src"
vmctl exec "msdev C:\\oat_src\\AT.DSW /MAKE \"AT - Win32 Release\" /OUT C:\\build.log"
vmctl read C:\\build.log                # stream log back to stdout
vmctl exec "C:\\GOG Games\\Airline Tycoon Deluxe\\AT.exe"
vmctl read C:\\at_exc.log               # crash log (if SEH fired)
```

## Protocol

Text-based, one command per line. Response always begins `OK` or `ERR`.
Commands that return data use `OK <length>\n<bytes>` (length-prefixed binary).
See the comment block at the top of `agent.c` for the full command list.

## Design notes

- **Why TCP instead of named pipes**: works across host-only networking
  without extra setup; same client could drive a remote VM.
- **Why BMP not PNG for screenshots**: zero dependencies. Convert to PNG
  host-side via `sips` (macOS built-in). The `vmctl shot-png` command does
  this transparently.
- **Why no auth**: trust boundary is the host-only network. Don't expose
  to a routable interface.
- **Why a single C file**: builds cleanly under VC6 with no extra deps,
  ships as a single ~50KB exe, runs on any XP install.

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

# ps_input_click.ps1 — canonical PowerShell synthetic click.
#
# Reference implementation of the same coupled-SendInput pattern the agent's
# `input.click` verb uses internally. Pack the cursor move and the button
# events into a SINGLE SendInput call, with the move event using
# MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK and
# coordinates normalised to the [0, 65535] virtual-desktop range.
#
# Why not the obvious shorter form?
#
#     [Win32]::SetCursorPos($x, $y)                  # 1) move cursor
#     [Win32]::mouse_event(LEFTDOWN, 0, 0, 0, 0)     # 2) click at "current"
#     [Win32]::mouse_event(LEFTUP,   0, 0, 0, 0)
#
# LEFTDOWN/LEFTUP without coordinate fields fire at whatever the cursor's
# position is at *event-processing time*, not at whatever was passed to
# SetCursorPos. Any drift between (1) and (2) — mouse hooks, the DWM thread,
# focus events, P/Invoke marshaling latency — translates directly into
# click-position drift. On Unity IMGUI surfaces and other per-frame-polled
# hitboxes, the click registers but lands on the wrong layer.
#
# The form below cannot drift. See agent-side `input.cpp` for the matching
# C++ implementation.
#
# Usage:
#   .\ps_input_click.ps1 -X 100 -Y 200
#   .\ps_input_click.ps1 -X 100 -Y 200 -Button right
#   .\ps_input_click.ps1 -X 100 -Y 200 -Button middle

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)] [int]$X,
    [Parameter(Mandatory=$true)] [int]$Y,
    [ValidateSet('left','right','middle')] [string]$Button = 'left'
)

$ErrorActionPreference = 'Stop'

Add-Type -Namespace 'Rh' -Name 'Input' -MemberDefinition @'
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT {
        public int dx;
        public int dy;
        public uint mouseData;
        public uint dwFlags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Explicit)]
    public struct INPUT {
        [FieldOffset(0)] public uint type;
        // INPUT_MOUSE = 0; on x64 the union starts at offset 8 due to
        // 8-byte alignment of dwExtraInfo (an IntPtr) inside MOUSEINPUT.
        [FieldOffset(8)] public MOUSEINPUT mi;
    }

    [DllImport("user32.dll", SetLastError = true)]
    public static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);

    [DllImport("user32.dll")]
    public static extern int GetSystemMetrics(int nIndex);
'@

# SM_X/YVIRTUALSCREEN = 76/77, SM_CX/CYVIRTUALSCREEN = 78/79.
$virtX = [Rh.Input]::GetSystemMetrics(76)
$virtY = [Rh.Input]::GetSystemMetrics(77)
$virtW = [Rh.Input]::GetSystemMetrics(78)
$virtH = [Rh.Input]::GetSystemMetrics(79)

# MOUSEEVENTF_*
$MOVE      = 0x0001
$ABSOLUTE  = 0x8000
$VIRTUALDESK = 0x4000
$LEFTDOWN  = 0x0002; $LEFTUP  = 0x0004
$RIGHTDOWN = 0x0008; $RIGHTUP = 0x0010
$MIDDLEDOWN = 0x0020; $MIDDLEUP = 0x0040

switch ($Button) {
    'left'   { $down = $LEFTDOWN;   $up = $LEFTUP }
    'right'  { $down = $RIGHTDOWN;  $up = $RIGHTUP }
    'middle' { $down = $MIDDLEDOWN; $up = $MIDDLEUP }
}

# Normalise (X, Y) to [0, 65535] across the virtual screen.
$dx = [int]((($X - $virtX) * 65535L + ($virtW / 2)) / $virtW)
$dy = [int]((($Y - $virtY) * 65535L + ($virtH / 2)) / $virtH)

$inputs = New-Object 'Rh.Input+INPUT[]' 3
$inputs[0].type = 0  # INPUT_MOUSE
$inputs[0].mi.dx = $dx
$inputs[0].mi.dy = $dy
$inputs[0].mi.dwFlags = $MOVE -bor $ABSOLUTE -bor $VIRTUALDESK
$inputs[1].type = 0
$inputs[1].mi.dwFlags = $down
$inputs[2].type = 0
$inputs[2].mi.dwFlags = $up

$sent = [Rh.Input]::SendInput(3, $inputs, [System.Runtime.InteropServices.Marshal]::SizeOf([type]'Rh.Input+INPUT'))
if ($sent -ne 3) {
    throw "SendInput injected $sent of 3 events (Win32 error $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
}

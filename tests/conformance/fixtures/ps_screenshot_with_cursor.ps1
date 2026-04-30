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

# ps_screenshot_with_cursor.ps1 — full-screen capture with the OS cursor
# composited onto the bitmap.
#
# Reference implementation of the same overlay the agent's `screen.capture`
# performs by default. The Win32 cursor is drawn by the system's separate
# cursor compositor on top of the desktop AFTER BitBlt (or PrintWindow)
# captures from a device context. So neither `BitBlt` nor `PrintWindow`
# includes the cursor by default. To put the cursor in the image, retrieve
# it via GetCursorInfo + GetIconInfo and DrawIconEx onto the captured
# bitmap's HDC at the cursor position minus its hotspot.
#
# Usage:
#   .\ps_screenshot_with_cursor.ps1 -OutPath shot.png
#   .\ps_screenshot_with_cursor.ps1 -OutPath shot.png -Region '100,100,800,600'

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)] [string]$OutPath,
    [string]$Region        # "x,y,w,h" — defaults to full virtual screen
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
Add-Type -Namespace 'Rh' -Name 'Cap' -MemberDefinition @'
    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int x; public int y; }

    [StructLayout(LayoutKind.Sequential)]
    public struct CURSORINFO {
        public int cbSize;
        public int flags;
        public IntPtr hCursor;
        public POINT ptScreenPos;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ICONINFO {
        public bool fIcon;
        public int xHotspot;
        public int yHotspot;
        public IntPtr hbmMask;
        public IntPtr hbmColor;
    }

    [DllImport("user32.dll")]
    public static extern bool GetCursorInfo(ref CURSORINFO pci);
    [DllImport("user32.dll")]
    public static extern bool GetIconInfo(IntPtr hIcon, out ICONINFO piconinfo);
    [DllImport("user32.dll")]
    public static extern bool DrawIconEx(IntPtr hdc, int xLeft, int yTop,
        IntPtr hIcon, int cxWidth, int cyWidth, uint istepIfAniCur,
        IntPtr hbrFlickerFreeDraw, uint diFlags);
    [DllImport("user32.dll")]
    public static extern int GetSystemMetrics(int nIndex);
    [DllImport("gdi32.dll")]
    public static extern bool DeleteObject(IntPtr ho);
'@

# Resolve capture rect.
if ($Region) {
    $parts = $Region.Split(',')
    if ($parts.Count -ne 4) { throw "Region must be 'x,y,w,h'" }
    $x = [int]$parts[0]; $y = [int]$parts[1]
    $w = [int]$parts[2]; $h = [int]$parts[3]
} else {
    $x = [Rh.Cap]::GetSystemMetrics(76)  # SM_XVIRTUALSCREEN
    $y = [Rh.Cap]::GetSystemMetrics(77)
    $w = [Rh.Cap]::GetSystemMetrics(78)
    $h = [Rh.Cap]::GetSystemMetrics(79)
}

# Capture the rect via System.Drawing (BitBlt under the hood).
$bmp = New-Object System.Drawing.Bitmap $w, $h
try {
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try {
        $g.CopyFromScreen($x, $y, 0, 0, [System.Drawing.Size]::new($w, $h),
                          [System.Drawing.CopyPixelOperation]::SourceCopy)

        # Composite the cursor onto the captured bitmap.
        $ci = New-Object 'Rh.Cap+CURSORINFO'
        $ci.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf([type]'Rh.Cap+CURSORINFO')
        if ([Rh.Cap]::GetCursorInfo([ref]$ci) -and ($ci.flags -band 0x1)) {  # CURSOR_SHOWING
            $ii = New-Object 'Rh.Cap+ICONINFO'
            if ([Rh.Cap]::GetIconInfo($ci.hCursor, [ref]$ii)) {
                try {
                    $hdc = $g.GetHdc()
                    try {
                        $drawX = $ci.ptScreenPos.x - $x - $ii.xHotspot
                        $drawY = $ci.ptScreenPos.y - $y - $ii.yHotspot
                        # DI_NORMAL = 0x3
                        [void][Rh.Cap]::DrawIconEx($hdc, $drawX, $drawY, $ci.hCursor,
                                                   0, 0, 0, [IntPtr]::Zero, 0x3)
                    } finally {
                        $g.ReleaseHdc($hdc)
                    }
                } finally {
                    if ($ii.hbmMask  -ne [IntPtr]::Zero) { [void][Rh.Cap]::DeleteObject($ii.hbmMask) }
                    if ($ii.hbmColor -ne [IntPtr]::Zero) { [void][Rh.Cap]::DeleteObject($ii.hbmColor) }
                }
            }
        }
    } finally {
        $g.Dispose()
    }
    $bmp.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Host "wrote $OutPath ($w x $h)"
} finally {
    $bmp.Dispose()
}

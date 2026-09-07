# Capture the port's window to a PNG.
#
# The framebuffer dump in src/rt64_context.cpp says what the game DREW; this
# says what the player SEES, and the two are not the same question. The
# Controller Pak prompt was drawn by the CPU straight into RDRAM with no display
# list at all, so the dump showed a finished picture while the window showed
# nothing -- a difference no counter in the port can express.
#
# Usage:  powershell -File tools/grab_window.ps1 out.png ["window title fragment"]
param(
    [string]$Out = "window.png",
    [string]$Match = "Rayman"
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win {
    [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string n);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    public struct RECT { public int L, T, R, B; }
    public struct POINT { public int X, Y; }
}
"@

$found = [IntPtr]::Zero
$title = ""
$cb = [Win+EnumProc]{
    param($h, $p)
    if ([Win]::IsWindowVisible($h)) {
        $sb = New-Object System.Text.StringBuilder 512
        [void][Win]::GetWindowText($h, $sb, 512)
        $t = $sb.ToString()
        if ($t -like "*$Match*") { $script:found = $h; $script:title = $t; return $false }
    }
    return $true
}
[void][Win]::EnumWindows($cb, [IntPtr]::Zero)

if ($found -eq [IntPtr]::Zero) { Write-Output "no window matching '$Match'"; exit 1 }

$r = New-Object Win+RECT
[void][Win]::GetClientRect($found, [ref]$r)
$tl = New-Object Win+POINT
[void][Win]::ClientToScreen($found, [ref]$tl)
$w = $r.R - $r.L; $h = $r.B - $r.T
if ($w -le 0 -or $h -le 0) { Write-Output "window has no client area"; exit 1 }

$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($tl.X, $tl.Y, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Output "captured '$title'  ${w}x${h} -> $Out"

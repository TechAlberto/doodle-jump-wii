# Screenshot a running Dolphin session.
#
# PrintWindow returns black for Dolphin's D3D swapchain, so instead the render
# window is moved on-screen and the desktop (which the DWM composites,
# including D3D content) is copied.  Works with a small virtual desktop.
#
# Dolphin lays the game image into a render surface that is shorter than the
# window client and stretches it, so a 640x480 client yields a squashed
# 640x395 picture.  measure_png.py undoes that; this script only records.
#
# -Count N -Interval S captures N frames in a single session (cheap way to
# watch a run progress); files get a -01..-NN suffix before the extension.
param(
    [Parameter(Mandatory = $true)][string]$Dol,
    [int]$Seconds = 8,
    [string]$Out = "$env:TEMP\opencode\dolphin-shot.png",
    [string]$WorkDir = "$env:TEMP\opencode\dolphin-capture",
    [string[]]$Config = @(),
    [int]$Count = 1,
    [double]$Interval = 1.0,
    [int]$ClientW = 640,
    [int]$ClientH = 565,
    [switch]$NoResize
)

$ErrorActionPreference = "Stop"
$exe = "C:\Users\spenc\Downloads\dolphin-2609a-x64\Dolphin-x64\Dolphin.exe"
$dolPath = (Resolve-Path $Dol).Path
Remove-Item $WorkDir -Recurse -Force -ErrorAction SilentlyContinue

$parts = @(('-u "{0}\user"' -f $WorkDir), ('-e "{0}"' -f $dolPath),
           '-C "Dolphin.Analytics.PermissionAsked=True"')
foreach ($c in $Config) { $parts += ('-C "{0}"' -f $c) }
$p = Start-Process -FilePath $exe -ArgumentList ($parts -join ' ') -PassThru

Add-Type @"
using System;using System.Text;using System.Drawing;using System.Runtime.InteropServices;
public class Shot {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  public static IntPtr FindRender(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h,l)=>{
      uint p; GetWindowThreadProcessId(h,out p);
      if (p==pid && IsWindowVisible(h)) {
        var sb=new StringBuilder(256); GetWindowText(h,sb,256);
        string t=sb.ToString();
        if (t.Contains("JIT") || t.Contains("Direct3D") || t.Contains("Vulkan") || t.Contains("OpenGL"))
        { found=h; return false; }
      }
      return true; }, IntPtr.Zero);
    return found;
  }
  public static Bitmap CaptureClient(IntPtr h) {
    RECT r; GetClientRect(h, out r);
    var pt = new POINT(); ClientToScreen(h, ref pt);
    int w = r.R - r.L, ht = r.B - r.T;
    var bmp = new Bitmap(w, ht);
    using (var g = Graphics.FromImage(bmp))
      g.CopyFromScreen(pt.X, pt.Y, 0, 0, new Size(w, ht));
    return bmp;
  }
}
"@ -ReferencedAssemblies System.Drawing

function Save-Shot($h, $path) {
    $bmp = [Shot]::CaptureClient($h)
    $w = $bmp.Width; $ht = $bmp.Height
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host ("saved {0} {1}x{2}" -f $path, $w, $ht)
}

Start-Sleep -Seconds $Seconds
$h = [Shot]::FindRender([uint32]$p.Id)
if ($h -eq [IntPtr]::Zero) { $h = $p.MainWindowHandle }
if ($h -ne [IntPtr]::Zero) {
    $sb = New-Object System.Text.StringBuilder 256
    [void][Shot]::GetWindowText($h, $sb, 256)
    Write-Host ("window: " + $sb.ToString())

    # Resize so the *client* area is exactly ClientW x ClientH: the window
    # frame eats a constant amount that we measure instead of guessing.
    if (-not $NoResize) {
        $wr = New-Object Shot+RECT
        $cr = New-Object Shot+RECT
        [void][Shot]::GetWindowRect($h, [ref]$wr)
        [void][Shot]::GetClientRect($h, [ref]$cr)
        $dx = ($wr.R - $wr.L) - ($cr.R - $cr.L)
        $dy = ($wr.B - $wr.T) - ($cr.B - $cr.T)
        $ow = $ClientW + $dx
        $oh = $ClientH + $dy
        # Bring the render window to the top-left so a small desktop can see it.
        [Shot]::SetWindowPos($h, [IntPtr]::Zero, 0, 0, $ow, $oh, 0x0040) | Out-Null
    }
    [Shot]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 1500

    $dir = Split-Path -Parent $Out
    $base = [System.IO.Path]::GetFileNameWithoutExtension($Out)
    $ext = [System.IO.Path]::GetExtension($Out)
    if (-not $dir) { $dir = "." }
    for ($i = 1; $i -le $Count; $i++) {
        if ($Count -eq 1) { $path = $Out }
        else { $path = Join-Path $dir ("{0}-{1:d2}{2}" -f $base, $i, $ext) }
        Save-Shot $h $path
        if ($i -lt $Count) { Start-Sleep -Milliseconds ([int]($Interval * 1000)) }
    }
} else {
    Write-Host "no render window found"
}
if (-not $p.HasExited) { $p.Kill() }

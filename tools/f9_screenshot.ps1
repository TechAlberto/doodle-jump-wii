# Debug helper: ask Dolphin itself for a screenshot (F9) instead of copying the
# desktop.  Dolphin's screenshots come from the emulated framebuffer, so they are
# not affected by window aspect correction or desktop scaling.
#
# Key injection is tried three ways, because a background process cannot always
# steal focus with SetForegroundWindow alone:
#   1. AttachThreadInput + SetForegroundWindow + SendKeys
#   2. PostMessage WM_KEYDOWN/WM_KEYUP to the Qt top-level and render windows
#   3. A synthetic left click (to focus) followed by the key messages
param(
    [string]$Dol = (Join-Path $PSScriptRoot "..\doodle-wii.dol"),
    [int]$Seconds = 8,
    [string]$WorkDir = "$env:TEMP\opencode\dolphin-f9",
    [switch]$KeepClosed
)

$ErrorActionPreference = "Stop"
$exe = "C:\Users\spenc\Downloads\dolphin-2609a-x64\Dolphin-x64\Dolphin.exe"
$dolPath = (Resolve-Path $Dol).Path
Remove-Item $WorkDir -Recurse -Force -ErrorAction SilentlyContinue

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Collections.Generic;
public class F9Win {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();

  public static string Title(IntPtr h) {
    var sb = new StringBuilder(512); GetWindowText(h, sb, 512); return sb.ToString();
  }
  public static string Class(IntPtr h) {
    var sb = new StringBuilder(256); GetClassName(h, sb, 256); return sb.ToString();
  }

  // Every visible top-level window owned by pid.
  public static List<IntPtr> TopLevel(uint pid) {
    var list = new List<IntPtr>();
    EnumWindows(delegate(IntPtr h, IntPtr l) {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid && IsWindowVisible(h)) list.Add(h);
      return true;
    }, IntPtr.Zero);
    return list;
  }

  // All descendants of h.
  public static List<IntPtr> Children(IntPtr h) {
    var list = new List<IntPtr>();
    EnumChildWindows(h, delegate(IntPtr c, IntPtr l) { list.Add(c); return true; }, IntPtr.Zero);
    return list;
  }

  public static void ForceForeground(IntPtr h) {
    ShowWindow(h, 9);                       // SW_RESTORE
    BringWindowToTop(h);
    uint targetTid = GetWindowThreadProcessId(h, IntPtr.Zero);
    uint myTid = GetCurrentThreadId();
    bool attached = false;
    if (targetTid != myTid) attached = AttachThreadInput(myTid, targetTid, true);
    try { SetForegroundWindow(h); }
    finally { if (attached) AttachThreadInput(myTid, targetTid, false); }
  }

  public static void KeyF9(IntPtr h) {
    PostMessage(h, 0x0100, (IntPtr)0x78, (IntPtr)0);            // WM_KEYDOWN VK_F9
    PostMessage(h, 0x0101, (IntPtr)0x78, unchecked((IntPtr)(int)0xC0000001)); // WM_KEYUP
  }

  public static void Click(IntPtr h, int x, int y) {
    IntPtr lp = (IntPtr)((y << 16) | (x & 0xFFFF));
    PostMessage(h, 0x0200, IntPtr.Zero, lp);                    // WM_MOUSEMOVE
    PostMessage(h, 0x0201, (IntPtr)1, lp);                      // WM_LBUTTONDOWN
    PostMessage(h, 0x0202, IntPtr.Zero, lp);                    // WM_LBUTTONUP
  }
}
"@

function Get-Shots {
    $dirs = @(
        (Join-Path $WorkDir "user\ScreenShots"),
        (Join-Path $WorkDir "user\Screen Shots")
    )
    foreach ($d in $dirs) {
        if (Test-Path $d) {
            $f = @(Get-ChildItem $d -File -Recurse -ErrorAction SilentlyContinue)
            if ($f.Count -gt 0) { return $f }
        }
    }
    return @()
}

$parts = @(('-u "{0}\user"' -f $WorkDir), ('-e "{0}"' -f $dolPath),
           '-C "Dolphin.Analytics.PermissionAsked=True"')
$p = Start-Process -FilePath $exe -ArgumentList ($parts -join ' ') -PassThru
Start-Sleep -Seconds $Seconds

try {
    $pidU = [uint32]$p.Id
    $tops = [F9Win]::TopLevel($pidU)
    if ($tops.Count -eq 0) { throw "no visible windows for pid $pidU" }

    $render = $tops | Where-Object { [F9Win]::Title($_) -match "Direct3D|OpenGL|Vulkan|Dolphin" } | Select-Object -First 1
    $main = $tops | Where-Object { [F9Win]::Title($_) -notmatch "Direct3D|OpenGL|Vulkan" } | Select-Object -First 1
    if (-not $render) { $render = $tops[0] }
    Write-Host ("windows: " + (($tops | ForEach-Object { "0x{0:x} '{1}'" -f [int64]$_, [F9Win]::Title($_) }) -join "; "))

    # --- strategy 1: real focus + SendKeys
    [F9Win]::ForceForeground($render)
    Start-Sleep -Milliseconds 600
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.SendKeys]::SendWait("{F9}")
    Start-Sleep -Seconds 2
    $shots = Get-Shots
    if ($shots.Count -eq 0 -and $main) { [F9Win]::ForceForeground($main); Start-Sleep -Milliseconds 400; [System.Windows.Forms.SendKeys]::SendWait("{F9}"); Start-Sleep -Seconds 2; $shots = Get-Shots }

    # --- strategy 2: posted key messages
    if ($shots.Count -eq 0) {
        Write-Host "strategy 1 failed, posting WM_KEYDOWN"
        foreach ($h in @($render, $main) + [F9Win]::Children($render) + [F9Win]::Children($main)) {
            if ($h -ne $null -and [int64]$h -ne 0) { [F9Win]::KeyF9($h) }
        }
        Start-Sleep -Seconds 2
        $shots = Get-Shots
    }

    # --- strategy 3: synthetic click first, then key
    if ($shots.Count -eq 0) {
        Write-Host "strategy 2 failed, clicking then posting key"
        [F9Win]::Click($render, 320, 240)
        Start-Sleep -Milliseconds 400
        [F9Win]::KeyF9($render)
        if ($main) { [F9Win]::Click($main, 200, 200); Start-Sleep -Milliseconds 300; [F9Win]::KeyF9($main) }
        Start-Sleep -Seconds 2
        $shots = Get-Shots
    }

    if ($shots.Count -gt 0) {
        foreach ($f in $shots) { Write-Host ("shot: " + $f.FullName) }
    } else {
        Write-Host "no screenshots captured"
    }
}
finally {
    if (-not $p.HasExited) { $p.Kill() }
}

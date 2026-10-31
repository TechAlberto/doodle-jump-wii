# Probe Dolphin's window layout (debug helper for the screenshot harness).
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class WinInfo {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  public static void Dump(IntPtr h, string pad) {
    var t = new StringBuilder(256); GetWindowText(h, t, 256);
    var c = new StringBuilder(256); GetClassName(h, c, 256);
    RECT wr, cr; GetWindowRect(h, out wr); GetClientRect(h, out cr);
    Console.WriteLine(pad + "hwnd=" + h + " vis=" + IsWindowVisible(h) + " cls=" + c + " title=" + t +
      " rect=" + wr.L + "," + wr.T + "," + wr.R + "," + wr.B +
      " client=" + (cr.R - cr.L) + "x" + (cr.B - cr.T));
  }
  public static void DumpChildren(IntPtr parent) {
    EnumChildWindows(parent, delegate(IntPtr h, IntPtr l) { Dump(h, "    "); return true; }, IntPtr.Zero);
  }
}
"@

$exe = "C:\Users\spenc\Downloads\dolphin-2609a-x64\Dolphin-x64\Dolphin.exe"
$dol = (Resolve-Path (Join-Path $PSScriptRoot '..\doodle-wii.dol')).Path
$work = "$env:TEMP\opencode\dolphin-probe"
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
$parts = @(('-u "{0}\user"' -f $work), ('-e "{0}"' -f $dol),
           '-C "Dolphin.Analytics.PermissionAsked=True"')
$p = Start-Process -FilePath $exe -ArgumentList ($parts -join ' ') -PassThru
Start-Sleep -Seconds 6

Add-Type -AssemblyName System.Windows.Forms
Write-Host ("screen: " + [System.Windows.Forms.Screen]::PrimaryScreen.Bounds)

[WinInfo]::EnumWindows({
    param($h, $l)
    $procId = 0
    [void][WinInfo]::GetWindowThreadProcessId($h, [ref]$procId)
    if ($procId -eq $p.Id -and [WinInfo]::IsWindowVisible($h)) {
        [WinInfo]::Dump($h, "")
        [WinInfo]::DumpChildren($h)
    }
    return $true
}, [IntPtr]::Zero) | Out-Null

if (-not $p.HasExited) { $p.Kill() }

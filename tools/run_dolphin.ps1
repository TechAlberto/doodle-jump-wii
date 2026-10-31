# Run a .dol in Dolphin, optionally dumping every presented frame to PNG.
#
#   .\tools\run_dolphin.ps1 -Dol .\doodle-wii.dol -Seconds 20
#   .\tools\run_dolphin.ps1 -Dol .\doodle-wii.dol -Seconds 20 -Gui
#
# Frames land in <WorkDir>\Dump\Frames.  Dolphin's -C keys are the layered
# config names ("Dolphin.Movie.DumpFrames"), not the GFX.ini section names.
param(
    [Parameter(Mandatory = $true)][string]$Dol,
    [int]$Seconds = 20,
    [string]$WorkDir = "$env:TEMP\opencode\dolphin-run",
    [switch]$Gui,
    [string[]]$Config = @(),
    [switch]$Keep
)

$ErrorActionPreference = "Stop"
$exe = "C:\Users\spenc\Downloads\dolphin-2609a-x64\Dolphin-x64\Dolphin.exe"
if (-not (Test-Path $exe)) { throw "Dolphin not found at $exe" }
$dolPath = (Resolve-Path $Dol).Path

if (-not $Keep) { Remove-Item $WorkDir -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force "$WorkDir\Dump\Frames" | Out-Null

$parts = @(
    ('-u "{0}\user"' -f $WorkDir),
    ('-e "{0}"' -f $dolPath),
    '-C "Dolphin.Movie.DumpFrames=True"',
    '-C "Dolphin.Movie.DumpFramesSilent=True"',
    ('-C "Dolphin.General.DumpPath={0}\Dump"' -f $WorkDir)
)
if (-not $Gui) { $parts += '-b' }
foreach ($c in $Config) { $parts += ('-C "{0}"' -f $c) }
$argLine = $parts -join ' '

Write-Host "dolphin $argLine"
$p = Start-Process -FilePath $exe -ArgumentList $argLine -PassThru
$deadline = (Get-Date).AddSeconds($Seconds)
$last = 0
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    Start-Sleep -Milliseconds 500
    $n = @(Get-ChildItem "$WorkDir\Dump\Frames" -Filter *.png -ErrorAction SilentlyContinue).Count
    if ($n -ne $last) { $last = $n }
}
$cpu = 0
try { $cpu = [math]::Round($p.TotalProcessorTime.TotalSeconds, 1) } catch {}
if (-not $p.HasExited) { $p.Kill() }
Start-Sleep -Milliseconds 800

$frames = @(Get-ChildItem "$WorkDir\Dump\Frames" -Filter *.png -ErrorAction SilentlyContinue)
Write-Host ("frames: {0}  cpu: {1}s  workdir: {2}" -f $frames.Count, $cpu, $WorkDir)
if ($frames.Count -gt 0) { $frames | Select-Object -First 3 Name, Length | Format-Table | Out-String | Write-Host }

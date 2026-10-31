# Build helper for Windows.  devkitPro's make dislikes spaces in paths, so a
# project under a path with spaces is temporarily mapped to a free drive letter.

param(
    [switch]$Clean,
    [string[]]$Extra
)

$ErrorActionPreference = "Stop"

$root = (Resolve-Path $PSScriptRoot).Path
$env:DEVKITPRO = "C:/devkitPro"
$env:DEVKITPPC = "C:/devkitPro/devkitPPC"
$env:PATH = "C:\devkitPro\devkitPPC\bin;C:\devkitPro\tools\bin;" + $env:PATH
$make = "C:\devkitPro\msys2\usr\bin\make.exe"

if (-not (Test-Path $make)) {
    throw "devkitPro not found at C:\devkitPro (install it from devkitpro.org)"
}

$buildArgs = @()
if ($Extra) {
    foreach ($item in $Extra) {
        foreach ($part in ($item -split '[,;]')) {
            $p = $part.Trim()
            if ($p) { $buildArgs += $p }
        }
    }
}

$mapped = $null
$workDir = $root
if ($root -match ' ') {
    foreach ($d in @("V", "W", "X", "Y", "Z")) {
        if (-not (Test-Path "${d}:\")) {
            & subst "${d}:" $root
            if ($LASTEXITCODE -eq 0) { $mapped = "${d}:"; break }
        }
    }
    if ($mapped) { $workDir = "${mapped}\" }
}

try {
    Push-Location $workDir
    if ($Clean) {
        & $make clean
        if ($LASTEXITCODE -ne 0) { throw "make clean failed" }
    }
    & $make @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}
finally {
    Pop-Location
    if ($mapped) { & subst $mapped /d | Out-Null }
}

Write-Host "built $(Join-Path $root 'doodle-wii.dol')"

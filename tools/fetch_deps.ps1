# SPDX-License-Identifier: GPL-2.0-or-later
# Downloads the third-party binaries PSPC builds against into third_party\ (git-ignored):
#   SDL2 2.32.10, Visual C++ development package, from the official libsdl-org release.
# Usage (from the repository root):  powershell -ExecutionPolicy Bypass -File tools\fetch_deps.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$third = Join-Path $root 'third_party'
$sdlDir = Join-Path $third 'SDL2-2.32.10'
$sdlUrl = 'https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-VC.zip'
$sdlSha256 = $null  # set to pin the archive; the size is checked below

if (Test-Path (Join-Path $sdlDir 'lib\x64\SDL2.lib')) {
    Write-Host "SDL2 already present in $sdlDir"
    exit 0
}
New-Item -ItemType Directory -Force $third | Out-Null
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('pspc_deps_' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $tmp | Out-Null
try {
    $zip = Join-Path $tmp 'SDL2-devel-2.32.10-VC.zip'
    Write-Host "Downloading $sdlUrl"
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -Uri $sdlUrl -OutFile $zip -UseBasicParsing
    if ((Get-Item $zip).Length -lt 1000000) { throw "SDL2 download looks truncated" }
    if ($sdlSha256 -and (Get-FileHash $zip -Algorithm SHA256).Hash -ne $sdlSha256) { throw "SDL2 archive hash mismatch" }
    Expand-Archive -Path $zip -DestinationPath $tmp
    $extracted = Join-Path $tmp 'SDL2-2.32.10'
    if (-not (Test-Path (Join-Path $extracted 'lib\x64\SDL2.lib'))) { throw "unexpected SDL2 archive layout" }
    Move-Item $extracted $sdlDir
    Write-Host "SDL2 installed in $sdlDir"
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

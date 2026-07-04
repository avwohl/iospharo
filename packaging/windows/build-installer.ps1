# build-installer.ps1 - Stage, sign, and package the iospharo Windows VM.
#
#   powershell -ExecutionPolicy Bypass -File packaging\windows\build-installer.ps1
#   ... -NoSign            # skip Authenticode signing (local test builds)
#   ... -SigningKit <dir>  # Azure Trusted Signing kit (default: env
#                          #   IOSPHARO_SIGNING_KIT, else the z80cpmw kit at
#                          #   C:\temp\in\z80cpmw-signing-kit)
#
# Flow: stage build-win artifacts -> sign iospharo.exe + TestLibrary.dll ->
# makensis -> sign the setup exe -> verify signatures -> print SHA256.
# The signing kit signs with the z80cpmw-public Trusted Signing profile
# (cert subject CN=Aaron Wohl — personal cert, shared across products by
# owner decision 2026-07-04; see docs/CODE_SIGNING notes in z80cpmw).
#
# NOTE: the staged file list here must match the File/Delete lists in
# iospharo.nsi.

[CmdletBinding()]
param(
    [string]$SigningKit = $(if ($env:IOSPHARO_SIGNING_KIT) { $env:IOSPHARO_SIGNING_KIT } else { 'C:\temp\in\z80cpmw-signing-kit' }),
    [switch]$NoSign,
    [int]$VersionMajor = 0,
    [int]$VersionMinor = 1,
    [int]$VersionBuild = 0
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$BuildWin = Join-Path $RepoRoot 'build-win'
$Staging  = Join-Path $PSScriptRoot 'staging'
$Dist     = Join-Path $RepoRoot 'dist'
$Version  = "$VersionMajor.$VersionMinor.$VersionBuild"

if (-not (Test-Path (Join-Path $BuildWin 'test_load_image.exe'))) {
    throw "build-win\test_load_image.exe not found - run scripts/build-windows.sh first"
}

# ---- 1. Stage -------------------------------------------------------------
Write-Host "== Staging $Staging" -ForegroundColor Cyan
Remove-Item -Recurse -Force $Staging -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $Staging | Out-Null
New-Item -ItemType Directory -Path $Dist -Force | Out-Null

Copy-Item (Join-Path $BuildWin 'test_load_image.exe') (Join-Path $Staging 'iospharo.exe')

$dlls = @(
    'libc++.dll', 'libdl.dll', 'libffi-8.dll', 'libunwind.dll',
    'libwinpthread-1.dll',
    'libfreetype-6.dll', 'libpng16.dll', 'zlib1.dll', 'libbz2-1.dll',
    'libharfbuzz-0.dll', 'libcairo-2.dll', 'libpixman-1-0.dll',
    'libfontconfig-1.dll', 'libexpat-1.dll',
    'TestLibrary.dll',
    'SDL2.dll'   # 255-byte text MARKER (not a real DLL) - see iospharo.nsi
)
foreach ($d in $dlls) { Copy-Item (Join-Path $BuildWin $d) $Staging }

Copy-Item (Join-Path $RepoRoot 'LICENSE') (Join-Path $Staging 'LICENSE.txt')
Copy-Item (Join-Path $RepoRoot 'third_party\windows-runtime-dlls\README.md') `
          (Join-Path $Staging 'THIRD-PARTY-LICENSES.txt')

@"
iospharo $Version - Pharo Smalltalk VM for Windows x64
======================================================

A clean C++ reimplementation of the Pharo VM with an x86-64 JIT.
Works with standard, unmodified Pharo 13 images.

Getting a Pharo image
---------------------
This installer ships only the VM.  Download a standard Pharo 13 image
(e.g. from https://pharo.org/download or https://files.pharo.org) and
keep the .image, .changes AND the matching Pharo*.sources file together
in one directory.  The .sources file is required.

Running
-------
    iospharo.exe C:\path\to\Pharo.image              (interactive GUI)
    iospharo.exe C:\path\to\Pharo.image eval "3 + 4" (headless one-liner)

Or double-click a .image file if you enabled the file association during
install.

Notes
-----
- Run from a normal Windows shell (cmd, PowerShell, double-click).  Do
  not launch from an MSYS2 *login* shell: it strips USERPROFILE/APPDATA,
  which breaks Pharo's settings resolver.
- SDL2.dll in this directory is a small placeholder on purpose; the VM
  has its display/input backend built in.  Do not replace it.
- License: MIT (LICENSE.txt).  Redistributed runtime libraries:
  THIRD-PARTY-LICENSES.txt.

Project: https://github.com/avwohl/iospharo
"@ | Set-Content -Encoding UTF8 (Join-Path $Staging 'README.txt')

Write-Host ("   staged {0} files" -f (Get-ChildItem $Staging).Count)

# ---- 2. Sign inner binaries ------------------------------------------------
# Only OUR binaries: the exe and TestLibrary.dll.  Third-party DLLs keep
# their upstream provenance, and SDL2.dll is a text file signtool would
# reject.
$signScript = Join-Path $SigningKit 'sign.ps1'
if (-not $NoSign) {
    if (-not (Test-Path $signScript)) { throw "signing kit not found: $signScript" }
    Write-Host '== Signing inner binaries (Azure Trusted Signing)' -ForegroundColor Cyan
    # One invocation per file: passing two positional paths makes sign.ps1's
    # parameter binder shove the second into -Verify (observed 2026-07-04).
    foreach ($f in @('iospharo.exe', 'TestLibrary.dll')) {
        & powershell -ExecutionPolicy Bypass -File $signScript (Join-Path $Staging $f)
        if ($LASTEXITCODE -ne 0) { throw "signing $f failed ($LASTEXITCODE)" }
    }
}

# ---- 3. Build installer -----------------------------------------------------
Write-Host '== makensis' -ForegroundColor Cyan
$makensis = "${env:ProgramFiles(x86)}\NSIS\makensis.exe"
if (-not (Test-Path $makensis)) { $makensis = 'makensis.exe' }
& $makensis /DVERSIONMAJOR=$VersionMajor /DVERSIONMINOR=$VersionMinor /DVERSIONBUILD=$VersionBuild `
    (Join-Path $PSScriptRoot 'iospharo.nsi')
if ($LASTEXITCODE -ne 0) { throw "makensis failed ($LASTEXITCODE)" }

$setup = Join-Path $Dist "iospharo-$Version-setup.exe"
if (-not (Test-Path $setup)) { throw "expected installer not found: $setup" }

# ---- 4. Sign + verify the installer -----------------------------------------
if (-not $NoSign) {
    Write-Host '== Signing installer' -ForegroundColor Cyan
    & powershell -ExecutionPolicy Bypass -File $signScript $setup
    if ($LASTEXITCODE -ne 0) { throw "installer signing failed ($LASTEXITCODE)" }

    Write-Host '== Verifying signatures' -ForegroundColor Cyan
    & powershell -ExecutionPolicy Bypass -File $signScript -Verify $setup
    if ($LASTEXITCODE -ne 0) { throw "installer signature verify failed" }
    & powershell -ExecutionPolicy Bypass -File $signScript -Verify (Join-Path $Staging 'iospharo.exe')
    if ($LASTEXITCODE -ne 0) { throw "inner exe signature verify failed" }
}

Write-Host "== Done: $setup" -ForegroundColor Green
Get-FileHash -Algorithm SHA256 $setup | Format-List

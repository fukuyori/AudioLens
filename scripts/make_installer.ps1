<#
.SYNOPSIS
    Packages an existing AudioLens build into an installer (Inno Setup 6).

.DESCRIPTION
    Packages only. It does not build - that is scripts\build.ps1's job. The two
    are kept apart because a packaging step that quietly builds leaves the
    question "which script do I build with?" without an answer. Here, a missing
    build stops the script and it tells you how to produce one.

    The version comes from CMakeLists.txt. Copying it into the .iss would mean
    two places to update, and eventually only one of them would be, producing an
    installer whose stated version and contents disagree. The project's rule is
    that CMakeLists.txt is the single source, and this keeps to it.

    ASCII only. See the note at the top of build.ps1 for why.

.EXAMPLE
    .\scripts\build.ps1
    .\scripts\make_installer.ps1
#>
[CmdletBinding()]
param(
    # Build configuration to package. Same names as scripts\build.ps1.
    [ValidateSet('release', 'debug')]
    [string]$Preset = 'release',

    # Output directory. Defaults to dist\ at the repository root.
    [string]$OutputDir
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

# --- version (the single source) ---
$cmakeLists = Join-Path $repo 'CMakeLists.txt'
$versionLine = Select-String -Path $cmakeLists -Pattern '^\s*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)' |
    Select-Object -First 1
if (-not $versionLine) {
    throw "Could not read the version from $cmakeLists"
}
$version = $versionLine.Matches[0].Groups[1].Value

# --- build output ---
$sourceDir = Join-Path $repo "build\$Preset\bin"
$appExe = Join-Path $sourceDir 'AudioLens.exe'
if (-not (Test-Path $appExe)) {
    throw ("GUI not found: $appExe" +
           "`n`nBuild it first:" +
           "`n    .\scripts\build.ps1$(if ($Preset -ne 'release') { " -Preset $Preset" })" +
           "`n`n(The GUI needs Qt. Without Qt the build skips it and no" +
           "`n AudioLens.exe is produced.)")
}

# Check the version the built binaries report against CMakeLists.txt. Bumping
# the source and forgetting to rebuild otherwise yields an installer with stale
# contents under a new name.
#
# The check asks the CLI to state its version. Reading the exe's version
# resource looks more natural, but CMake attaches no .rc, so ProductVersion is
# empty - and making that part of the condition made the check itself pass
# silently. It did: an installer named 9.9.9 was produced from a 0.2.0 build.
# AUDIOLENS_VERSION is compiled in, so only what the CLI prints is evidence of
# what the binaries actually contain.
$cli = Join-Path $sourceDir 'audiolens_passthrough.exe'
if (-not (Test-Path $cli)) {
    throw "Verification CLI not found: $cli"
}
$banner = (& $cli 2>&1 | Select-Object -First 1) -join ''
if ($banner -notmatch '([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "Could not read the version from the built binaries: $banner"
}
$builtVersion = $Matches[1]
if ($builtVersion -ne $version) {
    throw ("CMakeLists.txt says $version but the built binaries say $builtVersion." +
           "`n`nRebuild:" +
           "`n    .\scripts\build.ps1$(if ($Preset -ne 'release') { " -Preset $Preset" })")
}

# --- Inno Setup ---
$iscc = @(
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $iscc) {
    throw ("Inno Setup 6 not found." +
           "`nInstall it from https://jrsoftware.org/isdl.php")
}

if (-not $OutputDir) { $OutputDir = Join-Path $repo 'dist' }
if (-not (Test-Path $OutputDir)) { New-Item -ItemType Directory -Path $OutputDir | Out-Null }

$iss = Join-Path $repo 'installer\AudioLens.iss'

Write-Host ""
Write-Host "Version : $version  (CMakeLists.txt)" -ForegroundColor Cyan
Write-Host "Input   : $sourceDir"
Write-Host "Output  : $OutputDir"
Write-Host ""

& $iscc "/DAppVersion=$version" "/DSourceDir=$sourceDir" "/DOutputDir=$OutputDir" $iss
if ($LASTEXITCODE -ne 0) { throw "Building the installer failed." }

$setup = Join-Path $OutputDir "AudioLens-$version-setup.exe"
Write-Host ""
if (Test-Path $setup) {
    $mb = (Get-Item $setup).Length / 1MB
    Write-Host ("Done: {0} ({1:N1} MB)" -f $setup, $mb) -ForegroundColor Green
} else {
    Write-Host "Output not found: $setup" -ForegroundColor Yellow
}

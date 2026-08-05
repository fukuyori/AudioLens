# Configures and builds AudioLens with the toolchain that ships with Visual
# Studio, so no separate CMake/Ninja install is required.
#
#   .\scripts\build.ps1                # RelWithDebInfo
#   .\scripts\build.ps1 -Preset debug  # Debug
#
# ASCII only, deliberately. These scripts are run by both PowerShell 7 and
# Windows PowerShell 5.1, and the two disagree about the encoding of a file
# without a byte order mark: 5.1 reads it as the ANSI code page and turns any
# non-ASCII text into mojibake, which here means a parse error rather than a
# garbled message. Staying inside ASCII removes the question.
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release')]
    [string]$Preset = 'release',
    [switch]$Clean,
    # Path to a Qt kit, e.g. C:\Qt\6.11.1\msvc2022_64. Auto-detected when omitted.
    [string]$QtDir = '',
    [switch]$NoGui
)

$ErrorActionPreference = 'Stop'
# The repository root, which is where CMakeLists.txt lives and where build\
# belongs - one level up now that this script sits in scripts\.
$root = Split-Path -Parent $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere not found. Install Visual Studio with the C++ workload."
}

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsPath) {
    throw "No Visual Studio with the C++ toolset was found."
}

# Prefer the CMake and Ninja bundled with Visual Studio; fall back to PATH.
$cmake = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) {
    $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
}
if (-not $cmake) { throw "cmake not found." }

$ninja = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
if (-not (Test-Path $ninja)) {
    $ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source
}
if (-not $ninja) { throw "ninja not found." }

$buildDir = Join-Path $root "build\$Preset"
if ($Clean -and (Test-Path $buildDir)) {
    Remove-Item -Recurse -Force $buildDir
}

# Import the x64 native build environment (cl.exe, INCLUDE, LIB) into this
# session by running vcvars64.bat and copying back the variables it sets.
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found: $vcvars" }

& "${env:ComSpec}" /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        Set-Item -Path "env:$($matches[1])" -Value $matches[2]
    }
}

$buildType = if ($Preset -eq 'debug') { 'Debug' } else { 'RelWithDebInfo' }

# Locate a Qt kit built with the same toolchain as everything else. A mingw kit
# would configure but fail to link against the MSVC-built static libraries.
if (-not $NoGui -and -not $QtDir) {
    foreach ($root_ in @('C:\Qt', 'D:\Qt', "$env:USERPROFILE\Qt")) {
        if (-not (Test-Path $root_)) { continue }
        $kit = Get-ChildItem $root_ -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^\d+\.\d+' } |
            Sort-Object { [version]($_.Name) } -Descending |
            ForEach-Object { Join-Path $_.FullName 'msvc2022_64' } |
            Where-Object { Test-Path (Join-Path $_ 'lib\cmake\Qt6Widgets') } |
            Select-Object -First 1
        if ($kit) { $QtDir = $kit; break }
    }
}

$cmakeArgs = @(
    '-S', $root, '-B', $buildDir, '-G', 'Ninja',
    "-DCMAKE_MAKE_PROGRAM=$ninja",
    "-DCMAKE_BUILD_TYPE=$buildType",
    # The project declares CXX only, so CMAKE_C_COMPILER would only draw an
    # "unused variable" warning from CMake.
    '-DCMAKE_CXX_COMPILER=cl.exe'
)
# Passed on every run, not only when -NoGui. CMake caches the option, so
# setting it just the once left a later build without -NoGui still skipping the
# GUI - and saying nothing about it, because the option was never re-evaluated.
$cmakeArgs += "-DAUDIOLENS_BUILD_GUI=$(if ($NoGui) { 'OFF' } else { 'ON' })"

if ($NoGui) {
    Write-Host "GUI: skipped (-NoGui)"
} elseif ($QtDir) {
    $cmakeArgs += "-DCMAKE_PREFIX_PATH=$QtDir"
    Write-Host "Qt: $QtDir"
} else {
    Write-Host "Qt: not found. The GUI will be skipped." -ForegroundColor Yellow
}

& $cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

& $cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

# Qt is not on PATH, so the GUI needs its DLLs and platform plugin copied
# alongside the exe before it can start at all.
$guiExe = Join-Path $buildDir 'bin\AudioLens.exe'
if ((Test-Path $guiExe) -and $QtDir) {
    $windeployqt = Join-Path $QtDir 'bin\windeployqt.exe'
    if (Test-Path $windeployqt) {
        $deployArgs = @('--no-translations', '--no-system-d3d-compiler', '--no-opengl-sw')
        if ($Preset -eq 'debug') { $deployArgs += '--debug' } else { $deployArgs += '--release' }
        & $windeployqt @deployArgs $guiExe 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { Write-Host "windeployqt failed." -ForegroundColor Yellow }
    }
}

Write-Host ""
Write-Host "Build succeeded: $buildDir\bin" -ForegroundColor Green

# Configures and builds AudioLens with the toolchain that ships with Visual
# Studio, so no separate CMake/Ninja install is required.
#
#   .\build.ps1                # RelWithDebInfo
#   .\build.ps1 -Preset debug  # Debug
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
$root = $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere が見つかりません。Visual Studio (C++ ワークロード) をインストールしてください。"
}

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsPath) {
    throw "C++ ツールセットを備えた Visual Studio が見つかりません。"
}

# Prefer the CMake and Ninja bundled with Visual Studio; fall back to PATH.
$cmake = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) {
    $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
}
if (-not $cmake) { throw "cmake が見つかりません。" }

$ninja = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
if (-not (Test-Path $ninja)) {
    $ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source
}
if (-not $ninja) { throw "ninja が見つかりません。" }

$buildDir = Join-Path $root "build\$Preset"
if ($Clean -and (Test-Path $buildDir)) {
    Remove-Item -Recurse -Force $buildDir
}

# Import the x64 native build environment (cl.exe, INCLUDE, LIB) into this
# session by running vcvars64.bat and copying back the variables it sets.
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat が見つかりません: $vcvars" }

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
# GUI — and saying nothing about it, because the option was never re-evaluated.
$cmakeArgs += "-DAUDIOLENS_BUILD_GUI=$(if ($NoGui) { 'OFF' } else { 'ON' })"

if ($NoGui) {
    Write-Host "GUI: ビルドしません (-NoGui)"
} elseif ($QtDir) {
    $cmakeArgs += "-DCMAKE_PREFIX_PATH=$QtDir"
    Write-Host "Qt: $QtDir"
} else {
    Write-Host "Qt: 見つかりません。GUI はスキップされます。" -ForegroundColor Yellow
}

& $cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake の構成に失敗しました。" }

& $cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "ビルドに失敗しました。" }

# Qt is not on PATH, so the GUI needs its DLLs and platform plugin copied
# alongside the exe before it can start at all.
$guiExe = Join-Path $buildDir 'bin\AudioLens.exe'
if ((Test-Path $guiExe) -and $QtDir) {
    $windeployqt = Join-Path $QtDir 'bin\windeployqt.exe'
    if (Test-Path $windeployqt) {
        $deployArgs = @('--no-translations', '--no-system-d3d-compiler', '--no-opengl-sw')
        if ($Preset -eq 'debug') { $deployArgs += '--debug' } else { $deployArgs += '--release' }
        & $windeployqt @deployArgs $guiExe 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { Write-Host "windeployqt に失敗しました。" -ForegroundColor Yellow }
    }
}

Write-Host ""
Write-Host "ビルド成功: $buildDir\bin" -ForegroundColor Green

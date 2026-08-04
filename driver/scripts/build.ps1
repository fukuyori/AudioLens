# Builds the AudioLens virtual audio driver.
#
#   .\scripts\build.ps1                    # Debug x64
#   .\scripts\build.ps1 -Configuration Release
#
# Requires, beyond what the app needs:
#   - The "Windows Driver Kit" Visual Studio component, which is what registers
#     the WindowsKernelModeDriver10.0 platform toolset. The NuGet WDK supplies
#     headers and libraries but not the toolset, so both are needed.
#   - "C++ Spectre-mitigated libraries (Latest MSVC)". Driver projects enable
#     Spectre mitigation by default and will not link without them.
#
# See README.md for the exact installer command.
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$driverRoot = Split-Path $PSScriptRoot -Parent

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere が見つかりません。" }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "Visual Studio (C++) が見つかりません。" }

# The 64-bit MSBuild specifically. The 32-bit one drives the WDK's INF
# verification step through its x86 helper, and the WDK ships that DLL for x64
# and ARM64 only, so the build dies with "cannot load x86\InfVerif.dll".
$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\amd64\MSBuild.exe'
if (-not (Test-Path $msbuild)) { throw "64bit の MSBuild が見つかりません: $msbuild" }

# The toolset check is done up front because the MSB8020 that MSBuild emits
# otherwise does not say which component to install.
$toolset = Join-Path $vsPath "MSBuild\Microsoft\VC\v*\Platforms\$Platform\PlatformToolsets\WindowsKernelModeDriver10.0"
if (-not (Get-Item $toolset -ErrorAction SilentlyContinue)) {
    throw @"
WindowsKernelModeDriver10.0 ツールセットがありません。管理者 PowerShell で次を実行してください:

  & "`${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\setup.exe" modify ``
      --installPath "$vsPath" ``
      --add Component.Microsoft.Windows.DriverKit ``
      --add Microsoft.VisualStudio.Component.VC.Runtimes.x86.x64.Spectre ``
      --quiet --norestart
"@
}

# The NuGet WDK supplies the headers and libraries; Directory.Build.props picks
# them up out of .\packages.
$nuget = (Get-Command nuget -ErrorAction SilentlyContinue).Source
if (-not $nuget) { throw "nuget が見つかりません。`winget install Microsoft.NuGet` を実行してください。" }

& $nuget restore (Join-Path $driverRoot 'packages.config') -PackagesDirectory (Join-Path $driverRoot 'packages')
if ($LASTEXITCODE -ne 0) { throw "NuGet の復元に失敗しました。" }

& "${env:ComSpec}" /c "`"$vsPath\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}

$targets = if ($Clean) { 'Rebuild' } else { 'Build' }
& $msbuild (Join-Path $driverRoot 'AudioLensDriver.sln') `
    "/t:$targets" "/p:Configuration=$Configuration" "/p:Platform=$Platform" /m /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { throw "ドライバのビルドに失敗しました。" }

$out = Join-Path $driverRoot "$Platform\$Configuration\package"
Write-Host ""
if (Test-Path $out) {
    Write-Host "ビルド成功: $out" -ForegroundColor Green
    Get-ChildItem $out | ForEach-Object { "  $($_.Name)" }
} else {
    Write-Host "ビルドは成功しましたが、出力先が見つかりません。ソリューション構成を確認してください。" -ForegroundColor Yellow
}

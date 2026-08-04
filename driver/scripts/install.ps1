# Installs the AudioLens virtual audio device for testing.
#
# MUST be run from an elevated PowerShell.
#
#   .\scripts\install.ps1
#
# What it does, and why each step is needed:
#   1. Trusts the signer, so the install does not stop to ask about it.
#   2. Adds the driver package to the DriverStore.
#   3. Creates the root-enumerated device node. The device has no bus to be
#      discovered on, so nothing appears until it is created explicitly.
#
# Test signing must already be enabled and the machine rebooted; the script
# checks and tells you the command if it is not. This holds even when the
# package carries a real commercial code-signing signature: since Windows 10
# 1607 a kernel driver needs a Microsoft signature from the Partner Center to
# load, and no ordinary certificate substitutes for that. Test signing, not the
# certificate, is what lets the driver load here.
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64',
    [string]$HardwareId = 'ROOT\AudioLensVirtualAudio'
)

$ErrorActionPreference = 'Stop'

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
        ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "管理者権限で実行してください。"
}

$driverRoot = Split-Path $PSScriptRoot -Parent
$packageDir = Join-Path $driverRoot "$Platform\$Configuration\package"
$inf = Get-ChildItem $packageDir -Filter *.inf -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $inf) { throw "INF が見つかりません: $packageDir`n先に build.ps1 と sign.ps1 を実行してください。" }

# --- 1. test signing ---
$testsigning = (bcdedit /enum "{current}" | Select-String -Pattern '^testsigning\s+Yes') -ne $null
if (-not $testsigning) {
    Write-Host ""
    Write-Host "テスト署名が有効になっていません。次を実行して再起動してください:" -ForegroundColor Yellow
    Write-Host "  bcdedit /set testsigning on" -ForegroundColor Yellow
    Write-Host ""
    throw "テスト署名が無効のため中止しました。"
}

# --- 2. trust whatever certificate actually signed the catalog ---
# Read the signer out of the catalog rather than assuming a particular file, so
# that changing who signs does not mean editing this script.
$cat = Get-ChildItem $packageDir -Filter *.cat | Select-Object -First 1
if (-not $cat) { throw "カタログが見つかりません: $packageDir" }

$signature = Get-AuthenticodeSignature $cat.FullName
if (-not $signature.SignerCertificate) {
    throw "カタログが署名されていません。先に .\scripts\sign.ps1 を実行してください。"
}
$signer = $signature.SignerCertificate
Write-Host "署名者: $($signer.Subject)"

$cerDir = Join-Path $driverRoot 'signer'
New-Item -ItemType Directory -Force -Path $cerDir | Out-Null
$cer = Join-Path $cerDir 'signer.cer'
Export-Certificate -Cert $signer -FilePath $cer -Force | Out-Null

# TrustedPublisher is what stops the "do you trust this publisher" prompt, so
# it is imported either way.
Import-Certificate -FilePath $cer -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null
Write-Host "証明書を信頼しました: LocalMachine\TrustedPublisher"

# Root is only for a self-signed certificate, which has nothing above it to
# make its chain valid. Putting a publicly issued leaf there would instead
# install it as a trusted root authority on this machine — a real weakening of
# the trust store, not a step towards installing a driver.
$chain = New-Object System.Security.Cryptography.X509Certificates.X509Chain
$chainOk = $chain.Build($signer)
$selfSigned = $signer.Subject -eq $signer.Issuer

if ($selfSigned) {
    Import-Certificate -FilePath $cer -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null
    Write-Host "自己署名証明書のため Root にも入れました: LocalMachine\Root"
} elseif (-not $chainOk) {
    Write-Host "警告: 証明書チェーンを検証できません ($($chain.ChainStatus.Status -join ', '))" -ForegroundColor Yellow
    Write-Host "      中間証明書が不足している可能性があります。" -ForegroundColor Yellow
} else {
    Write-Host "証明書チェーンは検証済みです (Root への追加は不要)。"
}

# --- 3. add to the driver store ---
Write-Host "ドライバパッケージを追加: $($inf.Name)"
pnputil /add-driver $inf.FullName /install
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 259) { throw "pnputil /add-driver に失敗しました ($LASTEXITCODE)。" }

# --- 4. create the device node ---
$existing = Get-PnpDevice -ErrorAction SilentlyContinue |
    Where-Object { $_.HardwareID -contains $HardwareId }
if ($existing) {
    Write-Host "デバイスは既に存在します: $($existing.FriendlyName)"
} else {
    $kitRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots').KitsRoot10
    $devcon = Get-ChildItem (Join-Path $kitRoot 'Tools') -Recurse -Filter devcon.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1
    if (-not $devcon) {
        throw @"
devcon.exe が見つかりません (WDK の Tools 配下)。
手動で作成する場合: デバイス マネージャー → 操作 → レガシ ハードウェアの追加 →
一覧から選択 → サウンド、ビデオ、およびゲーム コントローラー → ディスク使用 →
$($inf.FullName)
"@
    }
    Write-Host "デバイスノードを作成: $HardwareId"
    & $devcon.FullName install $inf.FullName $HardwareId
    if ($LASTEXITCODE -ne 0) { throw "devcon install に失敗しました。" }
}

Write-Host ""
Write-Host "インストールが完了しました。" -ForegroundColor Green
Write-Host "サウンド設定に「AudioLens 仮想出力」が現れます。"
Write-Host "これを既定の出力デバイスにしたうえで、AudioLens の「取り込み元」に指定してください。"

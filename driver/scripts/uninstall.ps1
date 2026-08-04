# Removes the AudioLens virtual audio device and its driver package.
#
# MUST be run from an elevated PowerShell.
#
#   .\scripts\uninstall.ps1
#   .\scripts\uninstall.ps1 -RemoveCertificate    # also untrust the test cert
#
# Removing the device first and the package second matters: pnputil refuses to
# delete a package that a live device is still using.
[CmdletBinding()]
param(
    [string]$HardwareId = 'ROOT\AudioLensVirtualAudio',
    [switch]$RemoveCertificate
)

$ErrorActionPreference = 'Stop'

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
        ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "管理者権限で実行してください。"
}

# --- 1. remove the device ---
$devices = Get-PnpDevice -ErrorAction SilentlyContinue |
    Where-Object { $_.HardwareID -contains $HardwareId }
foreach ($device in $devices) {
    Write-Host "デバイスを削除: $($device.FriendlyName)"
    pnputil /remove-device $device.InstanceId
}
if (-not $devices) { Write-Host "対象のデバイスはありません。" }

# --- 2. remove the driver package ---
# The published name is the oemNN.inf that the DriverStore assigned; match on
# the original name so the right one is deleted.
$packages = pnputil /enum-drivers | Out-String
$matched = [regex]::Matches($packages, '(?ms)発行された名前:\s*(oem\d+\.inf).*?元のファイル名:\s*audiolens\.inf')
if ($matched.Count -eq 0) {
    $matched = [regex]::Matches($packages, '(?ms)Published Name:\s*(oem\d+\.inf).*?Original Name:\s*audiolens\.inf')
}
foreach ($m in $matched) {
    $oem = $m.Groups[1].Value
    Write-Host "ドライバパッケージを削除: $oem"
    pnputil /delete-driver $oem /uninstall /force
}
if ($matched.Count -eq 0) { Write-Host "対象のドライバパッケージはありません。" }

# --- 3. optionally untrust the signer ---
# Matched by thumbprint, from the copy install.ps1 exported. Matching by name
# instead would risk deleting an unrelated certificate that happens to share a
# subject, and this only ever runs elevated against the machine store.
if ($RemoveCertificate) {
    $cer = Join-Path (Split-Path $PSScriptRoot -Parent) 'signer\signer.cer'
    if (-not (Test-Path $cer)) {
        Write-Host "signer\signer.cer がないため、証明書はそのままにします。" -ForegroundColor Yellow
    } else {
        $thumb = (New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $cer).Thumbprint
        foreach ($store in 'Root', 'TrustedPublisher') {
            Get-ChildItem "Cert:\LocalMachine\$store" |
                Where-Object Thumbprint -eq $thumb |
                ForEach-Object {
                    Remove-Item $_.PSPath -Force
                    Write-Host "証明書を削除: LocalMachine\$store ($($_.Subject))"
                }
        }
    }
}

Write-Host ""
Write-Host "アンインストールが完了しました。" -ForegroundColor Green
Write-Host "テスト署名を戻す場合: bcdedit /set testsigning off  (再起動が必要)"

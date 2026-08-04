# Prepares a VMware VM for driver testing, from the host side.
#
#   .\scripts\vm_prepare.ps1 -Vmx "D:\home\vmware\Windows 11\Windows 11 x64.vmx"
#
# The host's own Secure Boot is deliberately left alone: turning it off there
# would change the whole machine's security posture and put BitLocker into
# recovery. A VM can have Secure Boot disabled in its own firmware settings with
# no effect outside it.
#
# What this does:
#   1. Takes a snapshot, so every later step is one click from being undone.
#   2. Disables Secure Boot in the VM's firmware.
#   3. Shares the built driver package into the guest as a read-only folder.
#
# The VM must be powered off: firmware settings and shared folders are read at
# power-on, and a snapshot of a stopped VM is far smaller than one that has to
# capture 16 GB of guest memory.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Vmx,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64',
    [string]$SnapshotName = 'AudioLens ドライバ検証前',
    [switch]$SkipSnapshot
)

$ErrorActionPreference = 'Stop'
$driverRoot = Split-Path $PSScriptRoot -Parent

if (-not (Test-Path $Vmx)) { throw "VM が見つかりません: $Vmx" }

$packageDir = Join-Path $driverRoot "$Platform\$Configuration\package"
if (-not (Test-Path $packageDir)) {
    throw "ドライバパッケージがありません: $packageDir`n先に .\scripts\build.ps1 を実行してください。"
}

# Builds no longer sign, so an unsigned package is the expected state right
# after building and worth catching here rather than three steps into the guest.
$cat = Get-ChildItem $packageDir -Filter *.cat -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $cat -or -not (Get-AuthenticodeSignature $cat.FullName).SignerCertificate) {
    throw "パッケージが署名されていません。`n先に .\scripts\sign.ps1 を実行してください (USB トークンが要ります)。"
}
Write-Host "署名者: $((Get-AuthenticodeSignature $cat.FullName).SignerCertificate.Subject)"

$vmrun = 'C:\Program Files\VMware\VMware Workstation\vmrun.exe',
         'C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe' |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vmrun) { throw "vmrun が見つかりません。" }

$running = & $vmrun list | Select-String -SimpleMatch $Vmx
if ($running) {
    throw "VM が実行中です。シャットダウンしてから再実行してください:`n  & '$vmrun' stop `"$Vmx`" soft"
}

# --- 1. snapshot ---
if (-not $SkipSnapshot) {
    Write-Host "スナップショットを作成: $SnapshotName"
    & $vmrun snapshot $Vmx $SnapshotName
    if ($LASTEXITCODE -ne 0) { throw "スナップショットの作成に失敗しました。" }
}

# --- 2 & 3. firmware and shared folder, written straight into the .vmx ---
# vmrun's addSharedFolder needs the VM running, and firmware settings are only
# read at power-on, so both are done by editing the configuration instead.
# Existing lines are dropped first so running this twice cannot leave two
# contradictory settings behind.
$drop = '^uefi\.secureBoot\.enabled|^sharedFolder\.maxNum|^sharedFolder0\.|^isolation\.tools\.hgfs\.disable'
$lines = @(Get-Content $Vmx | Where-Object { $_ -notmatch $drop })

$lines += 'uefi.secureBoot.enabled = "FALSE"'

# Read-only: the guest only ever copies the package out of it.
$lines += 'isolation.tools.hgfs.disable = "FALSE"'
$lines += 'sharedFolder.maxNum = "1"'
$lines += 'sharedFolder0.present = "TRUE"'
$lines += 'sharedFolder0.enabled = "TRUE"'
$lines += 'sharedFolder0.readAccess = "TRUE"'
$lines += 'sharedFolder0.writeAccess = "FALSE"'
$lines += 'sharedFolder0.expiration = "never"'
$lines += "sharedFolder0.hostPath = `"$packageDir`""
$lines += 'sharedFolder0.guestName = "AudioLensDriver"'

Set-Content -Path $Vmx -Value $lines -Encoding UTF8
Write-Host 'Secure Boot を無効化しました (uefi.secureBoot.enabled = "FALSE")'
Write-Host "共有フォルダーを設定しました: AudioLensDriver -> $packageDir"

Write-Host ""
Write-Host "ホスト側の準備が完了しました。" -ForegroundColor Green
Write-Host @"

次はゲスト (VM) の中で、管理者 PowerShell から:

  # 1. テスト署名を有効にして再起動
  #    証明書があってもこれは要ります。Windows 10 1607 以降、カーネルドライバは
  #    Partner Center 由来の Microsoft 署名がないと読み込まれないためです。
  bcdedit /set testsigning on
  Restart-Computer

  # 2. 再起動後、共有フォルダーからドライバをコピーして入れる
  #    (共有フォルダーは \\vmware-host\Shared Folders\AudioLensDriver に見えます)
  mkdir C:\AudioLens
  copy "\\vmware-host\Shared Folders\AudioLensDriver\*" C:\AudioLens\

  # 3. 署名者を信頼させる
  #    公式証明書 (DigiCert Trusted Root G4 配下) なのでルート証明書の導入は不要。
  #    TrustedPublisher だけ入れれば、インストール時の確認が出なくなります。
  `$sig = Get-AuthenticodeSignature C:\AudioLens\audiolens.cat
  `$sig.SignerCertificate.Subject      # 「CN=Noriaki Fukuyori...」であることを確認
  Export-Certificate -Cert `$sig.SignerCertificate -FilePath C:\AudioLens\signer.cer -Force
  Import-Certificate -FilePath C:\AudioLens\signer.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher

  # 4. インストール
  pnputil /add-driver C:\AudioLens\audiolens.inf /install

  # 5. デバイスノードを作る (devcon が無い場合はデバイス マネージャーから
  #    「レガシ ハードウェアの追加」→ ディスク使用 → audiolens.inf)

戻すときは VMware でスナップショット「$SnapshotName」に復元してください。
"@

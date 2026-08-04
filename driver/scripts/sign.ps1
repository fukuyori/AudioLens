# Signs the built driver with the real code-signing certificate.
#
#   .\scripts\sign.ps1
#   .\scripts\sign.ps1 -Thumbprint <拇印>     # 証明書を明示指定する
#
# Needs no elevation, but does need the SafeNet USB token plugged in: the
# private key lives on it, and signtool will raise the token's own password
# dialog. Run this yourself — an unattended build cannot get past that prompt.
#
# Signing does not by itself make Windows load the driver. On a machine with
# Secure Boot on, a kernel driver must carry a Microsoft-issued signature from
# the Partner Center regardless of who else signed it; see README.md. For the
# test VM, install.ps1 covers what is still needed.
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64',
    # Matched against the certificate subject. Kept as a name rather than a
    # thumbprint so that renewing the certificate does not break the script;
    # pass -Thumbprint when more than one match exists.
    [string]$SubjectMatch = 'CN=Noriaki Fukuyori',
    [string]$Thumbprint = '',
    # RFC 3161 timestamp. Without one, the signature stops verifying the day the
    # certificate expires rather than staying valid for what was signed while it
    # was current.
    [string]$TimestampUrl = 'http://timestamp.digicert.com'
)

$ErrorActionPreference = 'Stop'
$driverRoot = Split-Path $PSScriptRoot -Parent
$packageDir = Join-Path $driverRoot "$Platform\$Configuration\package"

if (-not (Test-Path $packageDir)) {
    throw "パッケージが見つかりません: $packageDir`n先に .\scripts\build.ps1 を実行してください。"
}

$kitRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue).KitsRoot10
if (-not $kitRoot) { throw "Windows Kits が見つかりません。" }

$binDir = Get-ChildItem (Join-Path $kitRoot 'bin') -Directory |
    Where-Object { $_.Name -match '^10\.' } | Sort-Object Name -Descending | Select-Object -First 1
$signtool = Join-Path $binDir.FullName 'x64\signtool.exe'
$inf2cat = Join-Path $kitRoot 'bin\x86\inf2cat.exe'
if (-not (Test-Path $inf2cat)) {
    $inf2cat = Join-Path $binDir.FullName 'x86\inf2cat.exe'
}
foreach ($tool in @($signtool, $inf2cat)) {
    if (-not (Test-Path $tool)) { throw "ツールが見つかりません: $tool" }
}

# --- pick the certificate ---
$candidates = @(Get-ChildItem Cert:\CurrentUser\My, Cert:\LocalMachine\My -ErrorAction SilentlyContinue |
    Where-Object {
        $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date) -and $_.NotBefore -le (Get-Date) -and
        ($_.EnhancedKeyUsageList.ObjectId -contains '1.3.6.1.5.5.7.3.3')
    })

if ($Thumbprint) {
    $cert = $candidates | Where-Object Thumbprint -eq $Thumbprint | Select-Object -First 1
    if (-not $cert) { throw "拇印 $Thumbprint の有効なコード署名証明書が見つかりません。" }
} else {
    $matched = @($candidates | Where-Object { $_.Subject -like "*$SubjectMatch*" })
    if ($matched.Count -eq 0) {
        throw "「$SubjectMatch」に一致するコード署名証明書が見つかりません。USB トークンを挿してください。"
    }
    if ($matched.Count -gt 1) {
        Write-Host "複数一致しました。-Thumbprint で 1 つに絞ってください:" -ForegroundColor Yellow
        $matched | ForEach-Object { Write-Host "  $($_.Thumbprint)  $($_.Subject)  (〜$($_.NotAfter.ToString('yyyy-MM-dd')))" }
        throw "証明書を特定できません。"
    }
    $cert = $matched[0]
}

# Print the full identity before touching anything: signing with the wrong
# certificate is not something you want to discover after the fact.
Write-Host "署名者 : $($cert.Subject)"
Write-Host "発行者 : $($cert.Issuer)"
Write-Host "拇印   : $($cert.Thumbprint)"
Write-Host "有効期限: $($cert.NotAfter.ToString('yyyy-MM-dd'))"
Write-Host ""

# --- 1. sign the .sys files ---
# Order matters. The catalog holds a hash of every file the INF lists, the .sys
# among them, so the .sys has to reach its final bytes before inf2cat runs.
# Building the catalog first and signing the .sys afterwards leaves the catalog
# describing a file that no longer exists.
$sysFiles = @(Get-ChildItem $packageDir -Include *.sys -Recurse)
if (-not $sysFiles) { throw ".sys が見つかりません: $packageDir" }

foreach ($file in $sysFiles) {
    Write-Host "署名: $($file.Name)"
    & $signtool sign /fd SHA256 /td SHA256 /tr $TimestampUrl /sha1 $cert.Thumbprint $file.FullName
    if ($LASTEXITCODE -ne 0) { throw "署名に失敗しました: $($file.Name)" }
}

# --- 2. build the catalog ---
$os = if ($Platform -eq 'ARM64') { '10_NI_ARM64' } else { '10_X64' }
Write-Host ""
Write-Host "カタログを生成: $packageDir"
& $inf2cat /driver:$packageDir /os:$os /uselocaltime
if ($LASTEXITCODE -ne 0) { throw "inf2cat に失敗しました。" }

# --- 3. sign the catalog ---
$catFiles = @(Get-ChildItem $packageDir -Include *.cat -Recurse)
if (-not $catFiles) { throw ".cat が生成されませんでした。" }

foreach ($file in $catFiles) {
    Write-Host "署名: $($file.Name)"
    & $signtool sign /fd SHA256 /td SHA256 /tr $TimestampUrl /sha1 $cert.Thumbprint $file.FullName
    if ($LASTEXITCODE -ne 0) { throw "署名に失敗しました: $($file.Name)" }
}

Write-Host ""
foreach ($file in ($sysFiles + $catFiles)) {
    & $signtool verify /pa /v $file.FullName 2>&1 | Select-String -Pattern 'Successfully|SignTool Error' |
        ForEach-Object { "  $($file.Name): $($_.Line.Trim())" }
}

Write-Host ""
Write-Host "署名が完了しました。管理者 PowerShell で .\scripts\install.ps1 を実行してください。" -ForegroundColor Green

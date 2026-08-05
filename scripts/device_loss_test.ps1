<#
.SYNOPSIS
    Exercises recovery from the loss of an audio endpoint (requirement N-03).

.DESCRIPTION
    AudioLens only runs its recovery path when Windows has actually destroyed
    the audio endpoint. Neither sleep nor a brief unplug does that: measured,
    both left the endpoint alive and showed up as a ring overflow instead
    (docs/m3.5-robustness-notes.md section 4.3.17). Destroying it means leaving
    the device unplugged until it disappears from the device list, and that is
    not something a number of seconds can express - you have to watch for it.

    This script does the watching. It reads the device list once a second and
    says when to unplug and when to plug back in.

    ASCII only. See the note at the top of build.ps1 for why.

.EXAMPLE
    .\scripts\device_loss_test.ps1
    .\scripts\device_loss_test.ps1 -DeviceName "USB2.0 Device"
#>
[CmdletBinding()]
param(
    # Display name of the device to unplug. A substring is enough.
    [string]$DeviceName = "USB2.0 Device",

    # Seconds to wait after the device disappears before plugging it back in,
    # counted from the moment it vanished, so the teardown has time to finish.
    [int]$SettleSeconds = 10,

    # Executable used to enumerate devices.
    [string]$Exe = "$PSScriptRoot\..\build\release\bin\audiolens_passthrough.exe"
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Exe)) {
    throw "Not found: $Exe`nRun .\scripts\build.ps1 first."
}

$logPath = Join-Path $env:APPDATA 'AudioLens\audiolens.log'

function Get-RenderDevices {
    # --list prints a render section then a capture section. Take the first one,
    # matched on the section markers rather than their text, so this keeps
    # working if the tool's headings are ever reworded.
    $lines = & $Exe --list 2>&1
    $out = @()
    $seenHeading = 0
    foreach ($line in $lines) {
        if ($line -match '^\s*==.*==\s*$') { $seenHeading++; continue }
        if ($seenHeading -eq 1 -and $line -match '^\s*\d+\.\s+(.+?)(\s+\[.+\])?\s*$') {
            $out += $Matches[1].Trim()
        }
    }
    return $out
}

function Wait-ForDevice {
    param([string]$Name, [bool]$WantPresent, [string]$Prompt)

    $start = Get-Date
    while ($true) {
        $present = @(Get-RenderDevices | Where-Object { $_ -like "*$Name*" }).Count -gt 0
        if ($present -eq $WantPresent) {
            return [Math]::Round(((Get-Date) - $start).TotalSeconds, 1)
        }
        $elapsed = ((Get-Date) - $start).TotalSeconds
        Write-Host ("`r{0}  ({1:N0} s)" -f $Prompt, $elapsed) -NoNewline
        Start-Sleep -Seconds 1
    }
}

Write-Host ""
Write-Host "=== Device loss recovery test ===" -ForegroundColor Cyan
Write-Host ""

# Confirm the target is there before starting.
$devices = Get-RenderDevices
$match = @($devices | Where-Object { $_ -like "*$DeviceName*" })
if ($match.Count -eq 0) {
    Write-Host "No render device matches '$DeviceName'." -ForegroundColor Red
    Write-Host "Current render devices:"
    $devices | ForEach-Object { Write-Host "  - $_" }
    Write-Host ""
    Write-Host "Pass part of one of the above as -DeviceName."
    exit 1
}
if ($match.Count -gt 1) {
    Write-Host "'$DeviceName' matches more than one device:" -ForegroundColor Red
    $match | ForEach-Object { Write-Host "  - $_" }
    Write-Host ""
    Write-Host "Make -DeviceName more specific."
    exit 1
}
$target = $match[0]

# Note where the log ends, so only what follows is shown at the end.
$logBefore = 0
if (Test-Path $logPath) { $logBefore = @(Get-Content $logPath).Count }

Write-Host "Target : $target"
Write-Host "Log    : $logPath"
Write-Host ""
Write-Host "AudioLens must be running with this device as its output." -ForegroundColor Yellow
Write-Host ""
Read-Host "Press Enter when ready"

Write-Host ""
Write-Host "[Step 1] Unplug $target." -ForegroundColor Green
Write-Host "         Waiting for it to leave the device list. If it does not"
Write-Host "         disappear, waiting longer will not help - press Ctrl+C"
Write-Host "         after a couple of minutes."
Write-Host ""
$goneAfter = Wait-ForDevice -Name $DeviceName -WantPresent $false -Prompt "         waiting for the unplug..."
Write-Host ""
Write-Host ("         Gone from the list after {0} s." -f $goneAfter) -ForegroundColor Green
Write-Host "         The endpoint is destroyed - this is the case under test." -ForegroundColor Green

Write-Host ""
Write-Host "[Step 2] Waiting $SettleSeconds s for the teardown to finish." -ForegroundColor Green
for ($i = $SettleSeconds; $i -gt 0; $i--) {
    Write-Host ("`r         {0} s left " -f $i) -NoNewline
    Start-Sleep -Seconds 1
}
Write-Host "`r                    "

Write-Host ""
Write-Host "[Step 3] Plug $target back in." -ForegroundColor Green
Write-Host ""
$backAfter = Wait-ForDevice -Name $DeviceName -WantPresent $true -Prompt "         waiting for the replug..."
Write-Host ""
Write-Host ("         Back after {0} s." -f $backAfter) -ForegroundColor Green

Write-Host ""
Write-Host "[Step 4] Giving AudioLens 30 s to reconnect." -ForegroundColor Green
Write-Host "         (10 fast attempts = 6 s, then slow attempts every 5 s)"
Start-Sleep -Seconds 30

Write-Host ""
Write-Host "=== Result ===" -ForegroundColor Cyan
Write-Host ("Time to disappear : {0} s" -f $goneAfter)
Write-Host ("Time to come back : {0} s (unplugged for about {1} s)" -f $backAfter, ($SettleSeconds + $backAfter))
Write-Host ""
Write-Host "--- log written during the test ---"
if (Test-Path $logPath) {
    $new = @(Get-Content $logPath) | Select-Object -Skip $logBefore
    if ($new.Count -eq 0) {
        Write-Host "  (nothing was written)" -ForegroundColor Yellow
    } else {
        $new | ForEach-Object { Write-Host "  $_" }
    }
}

Write-Host ""
Write-Host "--- how to read it (the log itself is in Japanese) ---" -ForegroundColor Cyan
Write-Host "  ... DEVICE_INVALIDATED      -> destroyed; this is the case under test"
Write-Host "  reconnected, attempt N      -> recovered; N is how many tries it took"
Write-Host "  device changed, retrying    -> re-armed after the retry budget ran out"
Write-Host "  gave up after 34 attempts   -> two minutes was not enough; raise the budget"
Write-Host "  none of the above           -> not destroyed; leave it unplugged longer"
Write-Host ""

<#
.SYNOPSIS
    Runs the eight-hour endurance test (requirements.md section 5, item 4).

.DESCRIPTION
    Watches a running AudioLens for glitches, crashes and leaks.

    The app records its own latency and dropout counters to
    %APPDATA%\AudioLens\audiolens.log every ten minutes, so this script does not
    duplicate that. What it adds is what a process cannot see about itself once
    it has died: memory, handles and threads, sampled from outside and written
    to a CSV, plus the fact of whether it was still alive at the end.

    Audio has to be playing for the whole run, otherwise the engine idles and
    the test proves nothing. Either leave something playing, or let this script
    generate a tone with -Tone.

.EXAMPLE
    .\scripts\endurance_test.ps1 -Hours 8 -Tone
    .\scripts\endurance_test.ps1 -Hours 8          # play your own audio
#>
[CmdletBinding()]
param(
    [double]$Hours = 8,

    # Sample interval in seconds.
    [int]$IntervalSeconds = 60,

    # Generate a tone into the capture device for the duration. Without this,
    # something else must be playing.
    [switch]$Tone,

    # Capture device the tone is played into. Must match AudioLens' input.
    [string]$ToneDevice = 'CABLE Input',

    # Tone level. Low enough to leave in a room, high enough to be unambiguously
    # non-silent in the capture counters.
    [double]$ToneLevelDb = -40,

    [string]$OutputDir
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $repo 'build\release\bin'
$logPath = Join-Path $env:APPDATA 'AudioLens\audiolens.log'

if (-not $OutputDir) { $OutputDir = Join-Path $repo 'dist' }
if (-not (Test-Path $OutputDir)) { New-Item -ItemType Directory -Path $OutputDir | Out-Null }
$csvPath = Join-Path $OutputDir 'endurance.csv'

$proc = Get-Process AudioLens -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) {
    throw ("AudioLens is not running." +
           "`n`nStart it, press Start, and run this again:" +
           "`n    $bin\AudioLens.exe")
}

# Where the log ends now, so only what this run writes is reported at the end.
$logBefore = 0
if (Test-Path $logPath) { $logBefore = @(Get-Content $logPath).Count }

# Start fresh: samples are appended as they are taken, so a run that ends
# unexpectedly still leaves everything it measured. Mixing two runs into one
# file would make the leak comparison meaningless.
if (Test-Path $csvPath) { Remove-Item $csvPath -Force }

$tone = $null
$toneExe = $null
$toneArgs = $null
if ($Tone) {
    $toneExe = Join-Path $bin 'audiolens_tone.exe'
    if (-not (Test-Path $toneExe)) { throw "Not found: $toneExe" }
    # A little longer than the run, so the tone never stops first and turns the
    # tail of the test into an idle measurement.
    $toneSeconds = [int]($Hours * 3600) + 120
    $toneArgs = @('--device', $ToneDevice, '--level', $ToneLevelDb,
                  '--freq', '440', '--duration', $toneSeconds)
    $tone = Start-Process -FilePath $toneExe -PassThru -WindowStyle Hidden -ArgumentList $toneArgs
}

$start = Get-Date
$end = $start.AddHours($Hours)
$samples = New-Object System.Collections.ArrayList

Write-Host ""
Write-Host "=== Endurance test ===" -ForegroundColor Cyan
Write-Host "Process  : AudioLens (PID $($proc.Id)), started $($proc.StartTime)"
Write-Host "Duration : $Hours h, sampled every $IntervalSeconds s"
Write-Host "Audio    : $(if ($Tone) { "tone $ToneLevelDb dBFS -> $ToneDevice" } else { 'supplied externally' })"
Write-Host "CSV      : $csvPath"
Write-Host "App log  : $logPath"
Write-Host ""
Write-Host "Ctrl+C stops early; what has been sampled is still written." -ForegroundColor Yellow
Write-Host ""

$died = $false
$sampleErrors = 0
$lastError = ''
$toneRestarts = 0

try {
    while ((Get-Date) -lt $end) {
        $p = Get-Process -Id $proc.Id -ErrorAction SilentlyContinue
        if (-not $p) {
            $died = $true
            Write-Host "`nAudioLens exited at $(Get-Date -Format 'HH:mm:ss')." -ForegroundColor Red
            break
        }

        # Nothing measured here is worth ending an eight-hour run over. Reading
        # HandleCount or enumerating Threads can fail for a moment while threads
        # come and go, and with ErrorActionPreference = Stop one such moment used
        # to terminate the script - which then killed the tone on its way out and
        # left the remaining hours running against silence. Sample failures are
        # counted and reported instead.
        try {
            $elapsed = ((Get-Date) - $start).TotalMinutes
            $row = [PSCustomObject]@{
                time         = (Get-Date -Format 'HH:mm:ss')
                elapsed_min  = [Math]::Round($elapsed, 2)
                working_mb   = [Math]::Round($p.WorkingSet64 / 1MB, 2)
                private_mb   = [Math]::Round($p.PrivateMemorySize64 / 1MB, 2)
                handles      = $p.HandleCount
                threads      = $p.Threads.Count
                tone_alive   = $(if (-not $Tone) { 'n/a' } elseif ($tone -and -not $tone.HasExited) { 'yes' } else { 'NO' })
            }
            $null = $samples.Add($row)

            # Appended per sample rather than written once at the end, so that
            # however this run finishes, what it measured is already on disk and
            # the file's timestamp says when it stopped.
            if ($samples.Count -eq 1) {
                $row | Export-Csv -Path $csvPath -NoTypeInformation
            } else {
                $row | Export-Csv -Path $csvPath -NoTypeInformation -Append
            }

            Write-Host ("`r{0,6:N0} min   working {1,7:N1} MB   private {2,7:N1} MB   handles {3,5}   threads {4,3}" -f `
                $elapsed, $row.working_mb, $row.private_mb, $row.handles, $row.threads) -NoNewline
        }
        catch {
            $sampleErrors++
            $lastError = $_.Exception.Message
            Write-Host "`nSample failed ($($_.Exception.GetType().Name)): $lastError" -ForegroundColor Yellow
        }

        # The stimulus has to outlast the measurement. If the tone died - it
        # generates for a fixed duration, and it is a process like any other -
        # the rest of the run would measure an idle engine and prove nothing,
        # so it is restarted and the fact is recorded.
        if ($Tone -and $tone -and $tone.HasExited -and ((Get-Date) -lt $end)) {
            $toneRestarts++
            Write-Host "`nTone stopped; restarting it." -ForegroundColor Yellow
            $remaining = [int](($end - (Get-Date)).TotalSeconds) + 120
            $toneArgs[-1] = $remaining
            $tone = Start-Process -FilePath $toneExe -PassThru -WindowStyle Hidden -ArgumentList $toneArgs
        }

        Start-Sleep -Seconds $IntervalSeconds
    }
}
finally {
    if ($tone -and -not $tone.HasExited) { $tone.Kill() }
}

Write-Host ""
Write-Host ""
Write-Host "=== Result ===" -ForegroundColor Cyan

if ($samples.Count -lt 2) {
    Write-Host "Too few samples to say anything." -ForegroundColor Yellow
    exit 1
}

$first = $samples[0]
$last = $samples[$samples.Count - 1]
$ran = $last.elapsed_min

Write-Host ("Ran for      : {0:N0} min ({1:N1} h) of {2:N1} h asked for" -f $ran, ($ran / 60), ($Hours * 60 / 60))
Write-Host ("Still alive  : {0}" -f $(if ($died) { 'NO - it exited' } else { 'yes' }))

if ($ran -lt ($Hours * 60 - 2)) {
    Write-Host ("Short run    : stopped {0:N0} min early - the result below covers only what was measured." -f `
        ($Hours * 60 - $ran)) -ForegroundColor Yellow
}
if ($sampleErrors -gt 0) {
    Write-Host ("Sample errors: {0} (last: {1})" -f $sampleErrors, $lastError) -ForegroundColor Yellow
}
if ($toneRestarts -gt 0) {
    Write-Host ("Tone restarts: {0} - audio stopped and was restarted; there were silent gaps." -f $toneRestarts) -ForegroundColor Yellow
}
Write-Host ""
Write-Host ("{0,-12} {1,>10} {2,>10} {3,>10}" -f 'metric', 'start', 'end', 'change')
foreach ($m in @('working_mb', 'private_mb', 'handles', 'threads')) {
    $delta = $last.$m - $first.$m
    Write-Host ("{0,-12} {1,10} {2,10} {3,10}" -f $m, $first.$m, $last.$m, ('{0:+#;-#;0}' -f $delta))
}

# A leak shows as growth that does not stop. Comparing the first and last
# samples alone cannot tell that apart from a process that grew once and then
# settled, which is normal, so the second half is compared with the first.
$half = [int]($samples.Count / 2)
$firstHalf = ($samples[0..($half - 1)] | Measure-Object private_mb -Average).Average
$secondHalf = ($samples[$half..($samples.Count - 1)] | Measure-Object private_mb -Average).Average
$growth = $secondHalf - $firstHalf
Write-Host ""
Write-Host ("Private memory, first half {0:N1} MB -> second half {1:N1} MB ({2:+#.#;-#.#;0} MB)" -f `
    $firstHalf, $secondHalf, $growth)
if ($growth -gt 5) {
    Write-Host "  Still climbing in the second half - look at the CSV before calling this a pass." -ForegroundColor Yellow
} else {
    Write-Host "  Settled." -ForegroundColor Green
}

Write-Host ""
Write-Host "--- what the app logged during the run ---"
if (Test-Path $logPath) {
    $new = @(Get-Content $logPath) | Select-Object -Skip $logBefore
    $health = @($new | Where-Object { $_ -match 'health:' })
    $trouble = @($new | Where-Object { $_ -match 'WARN|ERROR' })

    if ($health.Count -gt 0) {
        Write-Host ("  {0} heartbeat line(s). First and last:" -f $health.Count)
        Write-Host "    $($health[0])"
        Write-Host "    $($health[$health.Count - 1])"
    } else {
        Write-Host "  No heartbeat lines - was the engine actually running?" -ForegroundColor Yellow
    }

    # The engine's silence counter only fires when the endpoint delivers no
    # packets at all. A virtual cable keeps clocking and hands over zero-filled
    # packets instead, which the engine counts as audio, so "silence 0" says the
    # timing path was exercised throughout - not that anything was audible. Only
    # the tone, or what you were playing, can say that.
    Write-Host ""
    Write-Host "  Note: 'silence 0' means packets kept arriving, not that sound was playing."
    Write-Host "  The DSP is only exercised by real signal, so a silent stretch tests"
    Write-Host "  the buffering but not the processing load."

    Write-Host ""
    if ($trouble.Count -eq 0) {
        Write-Host "  No warnings or errors." -ForegroundColor Green
    } else {
        Write-Host ("  {0} warning(s)/error(s):" -f $trouble.Count) -ForegroundColor Yellow
        $trouble | Select-Object -First 20 | ForEach-Object { Write-Host "    $_" }
    }
}

Write-Host ""
Write-Host "--- criterion 4: no glitch, no crash, no leak ---" -ForegroundColor Cyan
Write-Host "  crash : the 'Still alive' line above"
Write-Host "  glitch: underrun/overrun in the heartbeat lines must stay at 0"
Write-Host "  leak  : private memory must not still be climbing in the second half"
Write-Host ""
Write-Host "  Latency is in the heartbeat lines too. Criterion 3 caps it at 100 ms"
Write-Host "  and the observed range reaches into the nineties, so the maximum"
Write-Host "  over eight hours is the number that settles it."
Write-Host ""

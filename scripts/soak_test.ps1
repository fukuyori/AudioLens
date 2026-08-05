# Runs the passthrough engine against a live tone for a sustained period, which
# is how the M1 exit criteria (no glitches, measurable clock drift) are checked.
#
#   .\scripts\soak_test.ps1 -Seconds 3600 -Capture 3 -Render 4
#
# The tone defaults to -60 dBFS: loud enough to be unambiguously non-silent in
# the capture counters, quiet enough not to disturb anyone in the room.
[CmdletBinding()]
param(
    [int]$Seconds = 300,
    [string]$Capture = '3',
    [string]$Render = '4',
    [string]$ToneDevice = '',
    [double]$ToneLevelDb = -60,
    [double]$ToneFreqHz = 1000,
    [string]$Preset = 'release'
)

$ErrorActionPreference = 'Stop'
$bin = Join-Path (Split-Path $PSScriptRoot -Parent) "build\$Preset\bin"

foreach ($exe in 'audiolens_passthrough.exe', 'audiolens_tone.exe') {
    if (-not (Test-Path (Join-Path $bin $exe))) {
        throw "$exe not found. Run .\scripts\build.ps1 first."
    }
}

# The tone has to land in whatever endpoint the passthrough tool is tapping.
if (-not $ToneDevice) { $ToneDevice = $Capture }

$tone = Start-Process -FilePath (Join-Path $bin 'audiolens_tone.exe') `
    -ArgumentList '--device', $ToneDevice, '--freq', $ToneFreqHz, `
                  '--level', $ToneLevelDb, '--duration', ($Seconds + 15) `
    -PassThru -WindowStyle Hidden

Write-Host "Tone:        $ToneFreqHz Hz / $ToneLevelDb dBFS -> device $ToneDevice (PID $($tone.Id))"
Write-Host "Passthrough: device $Capture -> $Render / $Seconds s"
Write-Host ""

try {
    & (Join-Path $bin 'audiolens_passthrough.exe') `
        --capture $Capture --render $Render --duration $Seconds 2>&1
}
finally {
    if (-not $tone.HasExited) { $tone.Kill() }
}

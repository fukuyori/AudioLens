# AudioLens 0.2.0

*[日本語](README.ja.md)*

A preset-based audio assistance app for Windows that adjusts whatever the PC is
playing so it is easier to hear.

- Repository: <https://github.com/fukuyori/AudioLens>
- The version lives in `project(... VERSION ...)` in `CMakeLists.txt` and nowhere
  else. It reaches the code as `AUDIOLENS_VERSION`, and is visible in the tray
  menu and from `AudioLens.exe --version`.

Pick a preset — Conversation, Lecture, Film, Late night, Game, plus Rock, Jazz,
Classical and Ambient — and it suppresses muddy bass, lifts the frequencies
speech needs, and evens out differences in loudness. No specialist knowledge is
required: three amounts (Bass, Speech clarity, Loudness range) control the
strength of the effect, and the output volume and left/right balance sit in the
same place.

## Status

**M3 (GUI) implemented.** Presets can be chosen from the Qt 6 GUI and the system
audio is corrected as you listen. A virtual audio device such as VB-Cable has to
be installed separately to serve as the capture source.

| Milestone | State |
|---|---|
| M1 engine | Implemented, measured |
| M2 DSP core | Implemented, measured |
| M3 GUI and preset storage | Implemented, confirmed on hardware |
| M3.5 robustness (N-03) | Implemented, **every path measured** (including resume from sleep and device loss) |
| N-04 default device recovery | Implemented, measured |
| M4 virtual device driver | **Cancelled.** Record of the work up to a successful build: [driver/README.md](driver/README.md) |
| M5 finishing | Not started |

**Personal use only, and no self-written kernel driver** (decided 2026-08-04). A
bug in kernel mode means a blue screen or a machine that will not boot, whereas
a virtual audio device is only the way sound gets in. A widely distributed driver
carrying a Microsoft signature is the safer choice, so VB-Cable and the like are
used as the capture source. The reasoning is in
[docs/requirements.md](docs/requirements.md) section 4.1.

Measured: the Film preset narrows the loudness range (EBU R128 LRA) from 17.99 to
13.13 LU. The music presets deliberately leave it alone (17.99 to 17.99 LU) —
the dynamics of a recording are the arrangement, so they are not touched. In
real time the CPU cost is 0.55 % of one core and the added latency is 2 ms.

## Building

Visual Studio with the C++ workload is enough; CMake and Ninja come with it, so
nothing else has to be installed. The GUI needs Qt 6 (the msvc2022_64 kit). It is
detected automatically under `C:\Qt` and similar, and if no kit is found the GUI
is skipped and everything else still builds.

```powershell
.\scripts\build.ps1                  # RelWithDebInfo -> build\release\bin
.\scripts\build.ps1 -Preset debug    # Debug          -> build\debug\bin
.\scripts\build.ps1 -Clean           # reconfigure from scratch
.\scripts\build.ps1 -QtDir C:\Qt\6.11.1\msvc2022_64   # point at a Qt kit
.\scripts\build.ps1 -NoGui           # skip the GUI
```

The scripts under `scripts\` are ASCII only. They are run by both PowerShell 7
and Windows PowerShell 5.1, and the two disagree about the encoding of a file
that carries no byte order mark, which turns non-ASCII text into a parse error
rather than a garbled message.

## Building an installer

Needs [Inno Setup 6](https://jrsoftware.org/isdl.php).

```powershell
.\scripts\build.ps1            # build first
.\scripts\make_installer.ps1   # then package it into dist\
```

Produces `dist\AudioLens-<version>-setup.exe`, around 34 MB.

- **It does not build.** It packages. If there is no build output it stops and
  says how to produce one. Building is `scripts\build.ps1`'s job, and a
  packaging step that quietly builds leaves "which script do I build with?"
  without an answer.
- **The version comes from `CMakeLists.txt`**, and is checked against the
  version the built binaries report. They have to agree, because bumping the
  source and forgetting to rebuild otherwise produces an installer with stale
  contents under a new name.
- **No administrator rights.** It installs under
  `%LOCALAPPDATA%\Programs\AudioLens` by default. Choosing Program Files
  elevates.
- **`.pdb` and `.ilk` are left out.** Together they are 187 MB, seven tenths of
  the build output.
- **The Visual C++ runtime** is installed only if it is missing.
- **A missing VB-Cable is reported** but does not stop the install; it can be
  added afterwards.
- **"Start with Windows" is not set by the installer.** The app manages that
  checkbox itself, and two writers of the same registry value disagree sooner or
  later.

Uninstalling asks whether to remove settings, presets and the log
(`%APPDATA%\AudioLens`).

## Running it

```powershell
$bin = ".\build\release\bin"

# The GUI, which is the normal way in
& $bin\AudioLens.exe

# --- everything below is for measurement and diagnosis ---

# List devices and presets
& $bin\audiolens_passthrough.exe --list
& $bin\audiolens_process.exe --list-presets

# Correct the system audio with a preset and send it to another device.
#   --capture takes a *render* device, which is tapped via loopback
#   --ab 8 toggles the processing every 8 seconds so the effect can be compared
& $bin\audiolens_passthrough.exe --capture "CABLE Input" --render "Headphones" --preset movie --ab 8

# --takeover makes the capture device the system default while it runs
& $bin\audiolens_passthrough.exe --capture "CABLE Input" --render "Headphones" --takeover

# Recovery when the default output was left on the virtual cable and there is no sound
& $bin\audiolens_passthrough.exe --set-default "Headphones"

# Exercise following a device change (N-03) by changing a sample rate and putting it back
& $bin\audiolens_passthrough.exe --invalidate "CABLE Input"

# Log the ring fill and the resampler trim every 20 ms to a CSV.
#   A diagnostic for watching the drift control in time. The five-second status
#   line aliases everything faster than ten seconds, so this resolution is what
#   the difference between a real effect and an artefact depends on.
& $bin\audiolens_passthrough.exe --capture "CABLE Input" --render "Headphones" --fill-log fill.csv

# Process a WAV offline and measure the effect
& $bin\audiolens_process.exe --input in.wav --output out.wav --preset movie

# Separate the contribution of each stage (mid/side, slow levelling, de-esser)
& $bin\audiolens_process.exe --input in.wav --output out.wav --preset movie --no-midside
& $bin\audiolens_process.exe --input in.wav --output out.wav --preset movie --no-autogain
& $bin\audiolens_process.exe --input in.wav --output out.wav --preset movie --no-deesser

# Unit tests (no hardware required, 111 of them)
& $bin\audiolens_tests.exe

# Process loopback spike (used to settle option E; the answer was no)
& $bin\audiolens_procloop.exe --list
& $bin\audiolens_procloop.exe --pid <PID> --auto-mute 6 --duration 14
```

Install a virtual audio device such as VB-Cable by hand, make it the system
default output, and name it to `--capture`. The self-written driver (M4) is
frozen, so this arrangement is the final one.

## Investigating a fault

The GUI writes to `%APPDATA%\AudioLens\audiolens.log`, rotating one generation at
a megabyte. **A fault that did not happen in front of you can only be found
here** — a windowed process has nowhere to send stderr, so previously nothing
was kept at all.

```powershell
Get-Content "$env:APPDATA\AudioLens\audiolens.log" -Tail 40
```

Unplugging a device, changing the default device and resuming from sleep all
appear here.

## Licence

**Apache License 2.0** ([LICENSE](LICENSE)), except for `driver/`, which is
**Microsoft Public License (MS-PL)**.

`driver/` derives from `audio/simpleaudiosample` in Microsoft's
[Windows-driver-samples](https://github.com/microsoft/Windows-driver-samples).
MS-PL 3(D) requires distribution in source form to remain under MS-PL, so that
part alone falls outside the Apache 2.0 grant. Keep the Microsoft copyright
notices in each file. See [NOTICE](NOTICE) and
[driver/LICENSE-MS-PL.txt](driver/LICENSE-MS-PL.txt).

## Documentation

**The documents below are in Japanese.** They are working notes — what was
measured, what was got wrong and what the numbers were — and they are written in
the language they were thought in. This README is the English entry point.

| Document | Contents |
|---|---|
| [docs/requirements.md](docs/requirements.md) | Requirements (functional, non-functional, acceptance criteria) |
| [docs/architecture.md](docs/architecture.md) | Architecture (approach, DSP design, roadmap) |
| [docs/m1-engine-notes.md](docs/m1-engine-notes.md) | M1 notes (drift correction, measured latency, known limits) |
| [docs/m2-dsp-notes.md](docs/m2-dsp-notes.md) | M2 notes (DSP design, measured preset effects, defects found) |
| [docs/m3-gui-notes.md](docs/m3-gui-notes.md) | M3 notes (why Qt, screen layout, settings storage, defects found) |
| [docs/m3.5-robustness-notes.md](docs/m3.5-robustness-notes.md) | M3.5 notes (resampler, drift control, following device changes) |
| [driver/README.md](driver/README.md) | M4 virtual device driver (licence, build prerequisites, test signing, the road to official distribution) |

## Repository layout

```
src/app/               Qt 6 GUI (main window, tray, settings storage)
src/common/            logging, COM/handle RAII wrappers, denormal handling
src/engine/            WASAPI capture and render, ring buffer, format conversion
src/dsp/               filters, compressor, limiter, mid/side, de-esser,
                       slow levelling, the DSP chain
src/core/              presets and the slider mapping
src/analysis/          loudness measurement (ITU-R BS.1770-4 / EBU R128)
src/audiofile/         WAV input and output
src/tools/passthrough/ capture -> correct -> render CLI
src/tools/process/     offline WAV processing and measurement CLI
src/tools/tone/        signal generator CLI (to a device or a WAV)
src/tools/procloop/    process loopback spike CLI (used to settle option E)
tests/                 offline unit tests
scripts/               build, installer, soak and recovery tests
installer/             Inno Setup script
docs/                  design documents
```

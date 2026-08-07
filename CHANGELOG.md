# Changelog

Notable changes to AudioLens, newest first.

The version is defined once, by `project(... VERSION ...)` in `CMakeLists.txt`,
and reaches the code from there as `AUDIOLENS_VERSION`. The tray menu and
`AudioLens.exe --version` report the same number, so a binary cannot claim a
version the build disagrees with.

Dates are the dates of the release commit.

## 0.3.1 — 2026-08-07

### Added

- **Routing from the command line.** `--output` and `--input` take a device by
  name, and part of the name is enough — the device id is a stable opaque string
  nobody can type. A name matching more than one device is refused with the
  candidates listed rather than guessed at: picking the wrong output device is a
  mistake nothing on screen reflects, and it is noticed later as silence. An
  exact name is tried before any substring, so a device whose whole name is
  contained in a longer one can still be chosen.

  ```
  --output <name>   --input <name>   --list-outputs
  ```

  `--list-outputs` prints what the two options accept, marking the current
  output, the current input and the system default. One list serves both,
  because the capture side taps a playback device through loopback.

  Routing is applied before everything else on the command line. Changing the
  output device applies whatever was remembered for it — preset, amounts, volume
  and balance (F-13) — so a `--volume` on the same line has to land after that
  or the profile would overwrite it.

- **"Start in the tray, without the window"** (F-37), beside "Start with
  Windows". Starting with Windows already passed `--minimized` — a window
  appearing over whatever you were doing at login is not what that setting is
  asking for — but launching AudioLens yourself always showed the window, and
  there was no way to say otherwise short of editing a shortcut.

  The setting had been half-present since 0.1.x: `startMinimized` was read from
  and written to the settings file and used by nothing at all. This connects it
  to the same path `--minimized` already took.

- `--status` now reports the capture device alongside the output device.

## 0.3.0 — 2026-08-07

### Added

- **Command-line control of the running instance** (F-36). Options are handed to
  the copy of AudioLens that is already running and take effect at once; if none
  is running, the ones that change something start it with them applied.

  ```
  --preset <id>   --volume <0-100>   --volume-step <±n>   --balance <-50..50>
  --bass --clarity --leveling <0-100>
  --on --off --toggle   --bypass on|off
  --status   --list-presets
  --show --hide --quit --minimized --verbose --version --help
  ```

  `--preset` accepts the id or the name shown on screen, in either language.
  Flags are applied in a fixed order rather than left to right, so
  `--bass 20 --preset movie` and `--preset movie --bass 20` mean the same thing:
  choosing a preset resets the three amounts, so it has to be applied first.
  Values outside their range and unknown options are rejected rather than
  rounded or ignored. Exit codes are `0` done, `1` not running or could not be
  carried out, `2` bad command line — except `--quit`, which returns `0` when
  nothing was running, because the state asked for already holds.

- **Single instance.** A second launch hands its command line to the first and
  exits, and a launch with no options at all brings the window to the front. The
  name of the control channel is what settles it: the pipe is created with
  `FILE_FLAG_FIRST_PIPE_INSTANCE`, so exactly one process can hold it. Asking
  first and starting second cannot settle the race, because there is a gap
  between another launch's `CreateProcess` and the moment it starts listening.

- **Optional PATH entry in the installer**, off by default. Uninstalling removes
  only that entry along with its separator, and keeps the existing value type
  (normally `REG_EXPAND_SZ`), so a PATH containing `%USERPROFILE%` survives. An
  elevated install writes the machine PATH, otherwise the user's.

### Changed

- **Two copies of AudioLens can no longer fight over the default output device.**
  Each would redirect it to its own capture device and hand it back on exit, and
  whichever exited second would restore a device the other had already replaced
  (requirement N-04).

- **The tray menu entry is a command, not a state.** It was checkable, and the
  check said "processing is running" while the label said what clicking would
  do. Together they showed a ticked "Stop" during playback, which reads as
  stopped. Whether processing runs is already in the tray icon's colour.

- **Milestone M5 met**: all four v1.0 success criteria, including a 10.5-hour
  run on real material with no dropouts and no measurable leak.

### Fixed

- The periodic health line printed the run-long ring water marks on every line
  rather than the ones for that interval, so a single early excursion was
  repeated for the rest of the night and later ones could not be seen at all.

- Emphasis in the Japanese Markdown is now spaced (`テキストを **強調** します`),
  which some renderers require in order to bold it at all.

## 0.2.2 — 2026-08-07

### Added

- A periodic **health line** in the log — latency, ring fill, dropout counters
  and drift — so that a long quiet run leaves a record. Without one, "eight
  hours without trouble" is indistinguishable from "stopped early".
- **Per-interval ring fill water marks** alongside the run-long ones, reset by
  whoever reads them.
- `scripts/endurance_test.ps1`, hardened so that a transient failure while
  sampling process counters no longer terminates the run — which used to also
  kill the tone and leave the rest of the night measuring silence.

### Changed

- F-08 and success criterion 3 now state the 100 ms latency ceiling for the
  **steady state**. A source switching streams — a track change — stops
  delivering briefly and then arrives late in a burst; absorbing that is the
  ring buffer's job, and the alternative to a transient rise in latency is a
  dropout. Requirement N-01 decides which of the two to accept.

## 0.2.1 — 2026-08-06

### Added

- **English and Japanese interface** (F-34). Follows the Windows language by
  default and can be set explicitly; the change takes effect on the next start.
  The Japanese catalogue is compiled into the executable, so there is no
  separate file for an installer to omit or a user to delete — the failure mode
  being an app that silently reverts to English, which nobody reports as a bug.

## 0.2.0 — 2026-08-05

### Added

- **Inno Setup installer.** Installs without administrator rights by default,
  omits `.pdb` and `.ilk` (187 MB, seven tenths of the build output), installs
  the Visual C++ runtime only when missing, and reports a missing VB-Cable
  without blocking the install.
- `README.ja.md`, and `scripts/` for the build, the installer, and the device
  loss and soak tests.

## 0.1.8 — 2026-08-05

### Added

- **Render hold-off priming.** The output stays silent until the ring first
  reaches its target, rather than draining a ring the capture side has not had
  time to fill. Bounded rather than unlimited, so a source that never delivers
  cannot leave the listener in silence — a thin ring still plays.
- `audiolens_passthrough --fill-log`, recording ring fill and resampler trim to
  CSV every 20 ms. A five-second display folds every component faster than ten
  seconds, which is enough to misread the drift control entirely.

## 0.1.7 — 2026-08-05

### Added

- A **silence-fill counter**. A bug that inserted silence in the *middle* of
  audio — audible as a click every period — stayed invisible for as long as this
  counter was: the run reported zero underruns and zero overruns while splicing
  76 ms of silence into every 30 seconds of sound.

### Changed

- Idle detection is now a fixed amount of *time* rather than a fixed number of
  wakes, so tuning the timeout does not change what counts as silence.
- Target ring fill raised from two capture periods to three.

## 0.1.6 — 2026-08-05

### Added

- **Left/right balance** (F-24), attenuating the near side only, and remembered
  per output device.
- **Default output device guard** (N-04), recording what AudioLens displaced
  before touching anything, so that a crash cannot lose it.

### Changed

- Level meters reworked.

## 0.1.5 — 2026-08-04

### Added

- **Presets linked to the output device** (F-13). Headphones and desk speakers
  want different corrections, and a volume that suits one is painful on the
  other.
- **Master output volume** (F-22), attenuation only, so passthrough keeps its
  0 ms latency.

## 0.1.4 — 2026-08-04

### Added

- Ring fill **high and low water marks**. The instantaneous fill is sampled by
  whoever asks, which misses exactly the moments that matter: the ring is only
  ever a problem at its extremes, and those are brief.

### Changed

- The ring is sized to at least six capture periods, and the capture wait is
  derived from the period WASAPI actually granted rather than the one requested.

## 0.1.3 — 2026-08-04

### Added

- **Follows device changes** (N-03): devices appearing and disappearing, the
  default output changing, and sample-rate changes.

### Changed

- Drift smoothing lengthened to about fifteen seconds. It was a third of that,
  which suits two real endpoints and is far too short for a virtual cable: a
  cable is fed by whatever application is playing rather than by a crystal, so
  its fill wanders some 25 ms either side of target with nothing wrong at all.

## 0.1.2 — 2026-08-04

### Added

- **De-esser**, scaled by the clarity slider — at a clarity of 0 the speech
  bands are flat and there is nothing extra to tame.
- `audiolens_process --no-deesser`, `--no-midside`, `--no-autogain` for
  separating what each stage contributes.

## 0.1.1 — 2026-08-04

### Added

- **Mid/side processing** and **slow levelling** to a LUFS target.
- **Four music presets**: rock, jazz, classical, ambient. These deliberately do
  not touch dynamics — on a record the gap between quiet and loud is the
  arrangement, and the record was already compressed once on purpose.

### Changed

- Licence is Apache 2.0, with `driver/` under the Microsoft Public License
  because it derives from Microsoft's Windows driver samples.

## 0.1.0 — 2026-08-04

Initial release.

- WASAPI loopback capture → DSP → render across two endpoints, which may run at
  different sample rates and always run on independent clocks. Drift is handled
  by trimming the resampler's ratio by a few parts per million to hold the ring
  at its target fill, rather than dropping or duplicating whole frames.
- DSP chain: biquad filters, K-weighting, compressor, limiter with lookahead.
- Preset system mapping three plain-language amounts — bass, clarity, levelling
  — onto the DSP parameters, so no frequency or decibel value is ever shown.
- Qt 6 GUI with tray residency.
- `audiolens_process` for offline WAV processing and loudness measurement
  (ITU-R BS.1770-4 / EBU R128), `audiolens_passthrough` for measurement on real
  devices, and an offline unit test suite needing no hardware.

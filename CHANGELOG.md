# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Controller mode (MIDI builds, `sp-mix-full-midi.bin`): FUNCTION + tap PLAY, or `mode ctrl` in the console. The faders and buttons send CC to the DAW (volume CC 20–23; balance 24–27, with PLAY or the track button held; mute 102–105 and solo 106–109 as button presses, 127 then 0), the DAW mixes, and the SP-1 passes the host's audio at 0 dB with only its master volume. The four volumes are sent on entering the mode, on a PLAY tap (`midi dump`) and when USB comes up. Faders have hysteresis and a 10 ms rate limit, values from the DAW are never sent back, and echoes within 300 ms of a control moving are ignored. The mode is not saved, and the mixer's state comes back when it is left. See `docs/MIDI.md`.

## [0.1.10] - 2026-10-26

Release files: `sp-mix-full.bin` is the mixer, as in 0.1.0. `sp-mix-full-midi.bin` is the same with MIDI control over USB on channel 1 (see `docs/MIDI.md`). `sp-mix-m0.bin` and `sp-mix-m1.bin` are bring-up builds. Settings saved by 0.1.0 should carry over: their format has not changed, and flashing leaves the settings area alone.

### Added

- `SP1M_STORAGE=low scripts/build.sh` (and `boards/teenage_engineering/sp1/sp1-storage-low.overlay`): an optional flash layout that keeps the settings in their own 16 KiB at 0xCA000-0xCDFFF instead of the default 0xF7000-0xFEFFF, which other SP-1 firmware also uses. The default layout is unchanged.
- Optional MIDI control over USB (`CONFIG_SP1M_MIDI`, off by default; receive only): track volume, balance, mute and solo, master volume and scene recall from a DAW, app or controller, on one channel, with 14-bit CC pairs. MIDI sets the same state as the controls, so it is saved, recalled and capped like them. See `docs/MIDI.md`. With the option off, the firmware image is unchanged. The release has it as `sp-mix-full-midi.bin`.

### Changed

- The project is now called SP-Mix (was SP-1 Mixer). The USB product name is "SP-Mix", the release files are `sp-mix-<variant>.bin` (was `sp1mixer-<variant>.bin`), the first line of `status` starts with `sp-mix` (the console page still reads the old `sp1mixer`), the test file is `sp-mix-test.wav`, and the west workspace directory is `sp-mix`, the default name of a clone.
- CI runs every host test suite (`ctest`), including the MIDI suites, and builds `full-midi`. A release made by hand puts its version in the firmware's `fw` string.

### Fixed

- `scripts/build.sh`: `SP1M_MIDI` and `SP1M_STORAGE=low` builds get their own build directories (`build-full-midi`, `build-full-low`, ...). Before, an option stayed in the cached build after it was dropped, so switching back silently kept it. If you built with one of these options before, delete `build-full` once.

### Known issues

- MIDI control has been tried on hardware on macOS only (track volume, takeover, 14-bit values, balance). Windows, the iPad, and mute, solo, master and scene recall over MIDI have not been tried on hardware.
- macOS enumerates the SP-1 (audio device, serial port, MIDI port), but playback on a Mac has not been checked.
- The other known issues of 0.1.0 still apply, except that flashing is now known to leave the settings in place.

## [0.1.0] - 2026-10-15

First public release. The prebuilt firmware is `sp1mixer-full.bin`, which is the `full` build.

### Added

- USB Audio Class 2.0 playback device: 48 kHz, 16-bit, 8 channels (four stereo tracks in a 7.1 layout) on an adaptive isochronous endpoint without a feedback endpoint. A CDC ACM serial console is part of the same composite device. Two patches to Zephyr v4.3.1 (`zephyr-patches/`) make 784-byte adaptive packets possible on the nRF52840.
- 8-to-2 mixer with per-track volume, mute, solo and stereo balance, a master volume, a 5 Hz DC blocker and a soft clipper.
- An asynchronous sample-rate converter from the USB clock to the codec crystal clock. The clock ratio is estimated from USB start-of-frame and I2S timestamps, and the output is 24-bit I2S.
- Headphone output (CS42L42) and speaker output (TAS2505, (L+R)/2), with jack detection and automatic switching.
- Volume safety: the master volume is capped at −20 dB at power-up and on headphone insertion, defaults to −24 dB, and is never raised by a scene recall. The headphone path has a fixed 12 dB attenuation (`CONFIG_SP1M_HP_ATTEN_DB`).
- Four scene slots and the last state stored in internal flash (NVS). Flash is only written while no audio is streaming. Faders use soft takeover after a recall or power-up.
- LED meters for the track input levels and the output level, and a status LED mode (USB, streaming, headphones, charging).
- Recovery into the bootloader by holding Track 1 + Track 4 for 3 s while running or with the `dfu` console command, in addition to the bootloader's own Track 1 + Track 4 at plug-in.
- Diagnostics: the watchdog is fed only while the audio and control threads make progress. A stall watcher resets before the watchdog does. Fault, stall and watchdog records survive resets. There are counters for USB, audio, codecs, control-loop timing and per-thread CPU use, and the firmware commit is shown in `status` (`fw`).
- Power handling: power off with FUNCTION, standby while on USB power, automatic power-off without a host, and battery charging on or off.
- Serial console commands, and a Web Serial console page for Chrome and Edge (`tools/sp1-console.html`) that can copy diagnostics for bug reports.
- Build variants `m0` (safety base), `m1` (USB probe) and `full`, host unit tests for the portable core, and test WAV generators (`tools/channel-test.py`, `tools/mixer-test.py`).

### Verified on hardware

- All three bootloader paths work. Watchdog, stall and crash records survive resets.
- Windows and iPad enumerate the SP-1 as an 8-channel device. Per-channel levels are correct, and all packets carry 48 frames with no gaps.
- Both codecs come up and the speaker plays (L+R)/2. Plugging in headphones switches the speaker off and caps the master volume at −20 dB. Faders, mute, solo, balance, scenes and LED meters work.
- In about 12 minutes of playback on Windows, including one stop and restart: 0 I2S errors, 0 underruns, an average of 495 µs and a maximum of 761 µs of processing per 1333 µs block, at most 1.63 ms between blocks, audio thread 38 % CPU and 63 % in total. Playback from an iPad also works.

### Known issues

- The 30-minute and 8-hour soak tests have not been run yet.
- Scene retention across power-off has not been tested. Whether scenes survive flashing a new firmware is unknown.
- The output latency has not been measured. The device-side estimate is about 9–10 ms.
- Windows: if the SP-1 re-enumerates (reflash, reboot, `selftest`, replug) while a player is open, the player's stream comes back as stereo and only Track 1 plays. Stop and start playback again, select the SP-1 explicitly as the output device, or use WASAPI exclusive mode or ASIO through FlexASIO. iPad apps reconfigure automatically.
- Windows shared mode applies its volume slider in software, so keep it at 100 % and use the SP-1's VOL buttons.
- The USB IDs default to the pid.codes test ID 1209:0001.
- macOS and Linux hosts have not been tested. MIDI, audio back to the host, the 3.5 mm analog input, Bluetooth and effects are not supported.

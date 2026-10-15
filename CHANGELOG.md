# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

# SP-Mix

SP-Mix is unofficial firmware for the teenage engineering SP-1 (nRF52840) that turns it into a class-compliant USB Audio Class 2 mixer with 8 inputs and 2 outputs. A host (Windows or iPad) plays four stereo tracks to the SP-1 over USB, and the SP-1's faders and buttons mix them live to stereo for the headphone jack (CS42L42) or, with no headphones plugged in, the built-in speaker (TAS2505, mono (L+R)/2). The host needs no driver. The firmware is built on Zephyr v4.3.1 with two small patches to Zephyr's USB stack.

## What's new

**2026-10-20: the project is now called SP-Mix.** From the next release the SP-1 shows up on the host as "SP-Mix", the release files are `sp-mix-<variant>.bin`, and the first line of `status` starts with `sp-mix`. v0.1.0 still uses the old name, SP-1 Mixer (`sp1mixer-full.bin`); the console page reads both. Windows may keep showing the old device name until the device is uninstalled in Device Manager. To build from source, clone into the default `sp-mix` directory.

**2026-10-20, since v0.1.0 (not in a release yet): optional settings location.** By default the settings (scenes and the last state) are stored in flash at 0xF7000–0xFEFFF. feldd keeps its profiles at the same address. If you switch an SP-1 between SP-Mix and other firmware, build with `SP1M_STORAGE=low scripts/build.sh full`. The settings then go in their own 16 KiB at 0xCA000–0xCDFFF, and neither firmware touches the other's data. Tested on hardware: a saved scene survived a reboot, and feldd's profiles were byte-identical after switching back to feldd. The default layout and the prebuilt `.bin` files are unchanged. The two layouts do not see each other's settings, so switching layouts starts from the defaults. See [Build](#build) and [CHANGELOG.md](CHANGELOG.md). Thanks to [@ryanmgilmore](https://github.com/ryanmgilmore) ([#2](https://github.com/chillingartist/sp-mix/pull/2)).

## Status

Version 0.1.0 is the first public release (see [CHANGELOG.md](CHANGELOG.md)). The firmware runs on the SP-1 with Windows and iPad hosts, but long-run testing is not finished.

Verified on hardware:

- **Safety base (milestone M0):** all three ways into the bootloader work (hold Track 1 + Track 4 for 3 s while the firmware runs, hold them while plugging in USB, or the `dfu` console command). Watchdog, stall and crash records survive the reset and are reported after the restart.
- **USB (M1):** Windows and iPad enumerate the SP-1 as an 8-channel playback device. Per-channel levels match the channel test file; all packets carry 48 frames and none are missing.
- **Audio output (M2):** both codecs come up, the speaker plays (L+R)/2, and plugging in headphones switches the speaker off and caps the master volume at −20 dB. Faders, mute, solo, balance, scenes and LED meters have been tested.
- **Timing:** in about 12 minutes of playback on Windows, including one stop and restart: 0 I2S errors, 0 underruns, an average of 495 µs and a maximum of 761 µs of processing per 1333 µs audio block, at most 1.63 ms between blocks, audio thread 38 % CPU, 63 % in total. The full build also works on an iPad.
- **Storage across updates:** flashing new firmware through the bootloader leaves the settings area alone. On 2026-10-19, feldd's data at 0xF7000–0xFEFFF, where SP-Mix keeps its settings by default, was byte-identical after flashing SP-Mix and then feldd again. A scene saved with the `SP1M_STORAGE=low` layout survived a reboot ([#2](https://github.com/chillingartist/sp-mix/pull/2)). An update from one SP-Mix version to another has not been checked directly.

Not done yet:

- 30-minute and 8-hour soak tests.
- Scene retention across a full power-off. A saved scene survives a reboot, but a power cycle has not been tested.
- The output latency has not been measured (the device-side estimate is about 9–10 ms).
- macOS and Linux hosts have not been tried.

## Features

- **8-channel USB audio:** USB Audio Class 2.0, 48 kHz, 16-bit, 8 channels (four stereo tracks) on an adaptive isochronous endpoint without a feedback endpoint. This fits the nRF52840's single 1 KiB isochronous buffer (784-byte packets). A USB serial port (CDC ACM) is part of the same device.
- **Mixer:** per-track volume, mute, solo and stereo balance, plus a master volume. Mixing is done in float, gain changes are ramped across each block to avoid zipper noise, and the master bus has a 5 Hz DC blocker followed by a soft clipper.
- **ASRC to the codec clock:** the codecs run from the SP-1's 3.072 MHz crystal, which cannot follow the USB host. An asynchronous sample-rate converter (polyphase Kaiser-windowed sinc, 20 taps, 128 phases, ±1000 ppm) bridges the two clocks. The ratio comes from hardware timestamps of USB start-of-frame and I2S events and is trimmed by a slow loop on the buffer fill. Output is 24-bit I2S.
- **Scenes and LED meters:** four scene slots (track volumes, mute, solo, balance, master volume, speaker mode) and the last state are kept in internal flash. Flash is only written while no audio is streaming. Faders use soft takeover after a scene recall or power-up. Track LEDs show each track's incoming level, and the four playback LEDs show the output level or the device status.
- **Headphone/speaker switching with volume caps:** jack detection switches the speaker off when headphones are plugged in. The master volume is capped at −20 dB at power-up and on every headphone insertion, defaults to −24 dB, and is never raised by a scene recall. The headphone path has an extra fixed 12 dB attenuation (configurable).
- **Diagnostics:** the watchdog is fed only while the audio and control threads make progress. A stall watcher records where the control loop got stuck and resets before the watchdog does. Crash, stall and watchdog records survive the reset. There are counters for USB packets, buffer fill, clock tracking, codecs, control-loop timing and per-thread CPU use.
- **Browser console:** `tools/sp1-console.html` talks to the serial port from Chrome or Edge (Web Serial). It shows live button and fader readings, runs a per-button test, shows the status and reset history, and copies the diagnostics with one click.
- **Power handling:** power off and on with FUNCTION, standby while on USB power, automatic power-off when no host is connected, and battery charging that can be switched on or off.

Optional, off by default: MIDI control over USB, receive only (see [docs/MIDI.md](docs/MIDI.md)).

Not included: MIDI output, audio back to the host, the 3.5 mm analog input, Bluetooth and effects. Android is not a target platform.

## Disclaimer

- This is an unofficial community project. It is not affiliated with or endorsed by teenage engineering. SP-1 is a trademark of teenage engineering and is used here only to name the hardware this firmware runs on.
- Flashing third-party firmware is at your own risk. Before you flash, keep a firmware image you can go back to and learn the [recovery paths](#recovery-and-updates). The SP-1 has no reset button.
- Protect your hearing. The firmware caps the volume at power-up and when headphones are plugged in, but start with the volume low and raise it slowly, especially on headphones.
- The software is provided "as is", without warranty of any kind. See [LICENSE](LICENSE).

## Requirements

- **Hardware:** a teenage engineering SP-1 and a USB cable that fits the host. An iPad with a Lightning port needs a Lightning to USB camera adapter. The SP-1 declares 100 mA of bus power because these adapters reject devices that declare more.
- **Host:** Windows 10 (version 1703 or later, which includes the USB Audio 2.0 class driver) or Windows 11, or iPadOS. You also need an app that can send audio to individual output channels, such as Reaper (WASAPI, or any ASIO host through FlexASIO) on Windows or AUM on iPad. Any player that handles 8-channel WAV files works for the test files.
- **Building from source:** the Zephyr SDK 0.17.x (Arm toolchain), west, CMake and Ninja, set up as described in Zephyr's [Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html). The test file generators need Python 3, and the host unit tests need a C compiler with AddressSanitizer support.

## Getting started

### Install the firmware

1. Download `sp-mix-full.bin` from the [Releases](https://github.com/chillingartist/sp-mix/releases) page (in v0.1.0 the file is called `sp1mixer-full.bin`).
2. Put the SP-1 into its bootloader by holding **Track 1 + Track 4** while you plug in the USB cable. Keep holding until the updater sees the device. This is a feature of the SP-1's bootloader and works with any firmware installed.
3. Write the `.bin` file with the [Solderless web updater](https://solderless.engineering), a web tool that flashes the SP-1 through its bootloader, and follow its instructions.
4. The SP-1 restarts into the mixer. The LEDs light up briefly, then the track LEDs stay dimly lit. The host now sees a USB audio device with 8 output channels and a serial port.

### Recovery and updates

There are three ways to get into the bootloader, for example to install an update or go back to another firmware:

| Path | How | Notes |
| --- | --- | --- |
| 1 | Hold **Track 1 + Track 4** for 3 s while SP-Mix runs | The track LEDs fill up. Keep holding until the updater sees the SP-1. Works in every mode. |
| 2 | Hold **Track 1 + Track 4** while plugging in USB | Built into the bootloader, so it works whatever firmware is installed, including a broken one. |
| 3 | Type `dfu` in the [serial console](#serial-console) | No buttons needed. If the updater does not see the SP-1 afterwards, use path 2. |

If the firmware hangs, the bootloader's watchdog (about 5 s) restarts it. Path 2 is always available.

### Power

- **Power off:** hold FUNCTION on its own for 3 s while the playback LEDs count down. With USB connected, the SP-1 goes into standby and only shows the charge state. Hold FUNCTION for 0.6 s to start it again.
- **Power on from battery:** press FUNCTION.
- **Automatic power-off:** the SP-1 switches itself off when no USB host is connected and nothing is touched for 10 minutes.

## Host setup

The SP-1 appears as one 8-channel playback device with a 7.1 channel layout. The channels carry no track names, so use this table:

| Channels | 7.1 position | Track |
| --- | --- | --- |
| 1–2 | Front left / front right (FL/FR) | Track 1 |
| 3–4 | Center / LFE (FC/LFE) | Track 2 |
| 5–6 | Back left / back right (BL/BR) | Track 3 |
| 7–8 | Side left / side right (SL/SR) | Track 4 |

### Windows

- Windows uses its built-in USB Audio 2.0 class driver, so there is nothing to install.
- Keep the Windows volume for the SP-1 at **100 %**. In shared mode it is a software volume applied before the audio reaches the SP-1. Set the level with the SP-1's VOL buttons instead.
- To mix live, send four stereo buses to output pairs 1–2, 3–4, 5–6 and 7–8. You can do this in Reaper with WASAPI, or in an ASIO host through FlexASIO.
- Channel 4 is the LFE position. In the 1 kHz channel test on Windows, it arrived at its expected level.
- **Only Track 1 plays after the SP-1 re-enumerates.** If the SP-1 re-enumerates while a player is open (after a reflash, reboot, `selftest` or replug), Windows moves the player's existing stream back as stereo, so only Track 1 (channels 1–2) plays. To fix this, stop playback and start it again. You can also select the SP-1 explicitly as the app's output device, or use WASAPI exclusive mode or ASIO through FlexASIO.

### iPad

- Use an app that routes audio to individual output channels, such as AUM. Apps that only output stereo will normally use channels 1–2 (Track 1).
- After the SP-1 re-enumerates, iPad apps reconfigure automatically.
- If the iPad struggles to power the SP-1, `charge off` in the serial console switches off battery charging. The setting is saved.

## Controls

| Control | Action |
| --- | --- |
| Fader 1–4 | Track volume. The bottom ~2 % of travel is silent. |
| Tap a track button | Mute or unmute the track. The tap acts on release and counts only if released within 0.6 s. |
| FUNCTION + tap a track button | Solo on or off. Several tracks can be soloed, and solo overrides mute. |
| Hold a track button + move its fader | Balance (relative). It starts after a short movement, and about a third of the fader travel goes from center to one side. When you let go, the fader returns to volume with soft takeover. |
| VOL+ / VOL− | Master volume in 1 dB steps from −60 dB to 0 dB, plus off. Hold to repeat. The playback LEDs show the level while it changes. |
| Tap PLAY | Turn the scene layer on or off. The playback LEDs breathe while it is on, and it closes after 3 s without a button press. |
| In the scene layer: tap track button N | Recall scene N |
| In the scene layer: FUNCTION + tap track button N | Save the current state to scene N. The track LED flashes to confirm. |
| Tap FWD or BACK | Switch the LED mode between meter and status |
| FUNCTION + tap FWD or BACK | Switch the speaker mode between auto and off |
| Hold FUNCTION alone for 3 s | Power off |
| Hold Track 1 + Track 4 for 3 s | Enter the bootloader (works in every mode) |

**Soft takeover:** after power-up or a scene recall, a fader that does not match the stored volume blinks its track LED slowly. It takes over once it reaches the stored value. If the fader is below the stored value, pulling it down takes over at once, so moving a fader down never makes a track louder.

**Speaker mode:** in `auto` mode, the speaker plays whenever no headphones are plugged in. In `off` mode, it stays silent.

**LEDs:**

- Track LEDs show the level the host sends for that track, before the fader. A track LED is off when the track is muted or silenced by another track's solo. It blinks fast when the track is soloed and slowly while its fader waits for soft takeover. In the scene layer, the track LEDs instead show which slots hold a scene (dim) and the scene last saved or recalled (bright).
- Playback LEDs in meter mode show the output level, with steps at −30, −18, −9 and −3 dBFS. In status mode, LEDs 1–4 show USB connected, streaming, headphones in, and charging (blinking). All four flash briefly on a buffer underrun.

## Serial console

Next to the audio device, the SP-1 shows up as a USB serial port (CDC ACM).

- **Web console:** save `tools/sp1-console.html`, open it in Chrome or Edge (Web Serial is required), click **Connect SP-1** and pick the SP-1's port. Close any other serial program first. The page shows live button and fader readings, a per-button test, the device status with a reset history and a diagnostics log of the last two minutes, and a terminal. It reconnects after the SP-1 resets. If it does not, click **Connect SP-1** again.
- **Any terminal** (PuTTY, screen, ...) also works, at any baud rate. Lines end with CR or LF. The firmware does not echo what you type, so turn on local echo.

| Command | Action |
| --- | --- |
| `status` | One snapshot of all counters (see below) |
| `watch on\|off` | Print `status` every second |
| `adc` | Raw readings of the button ladders, faders and battery, plus the decoded buttons |
| `threads` | CPU % per thread since the previous `status` or `threads`, and free stack per thread |
| `clear` | Reset the USB and audio counters |
| `scene save\|load <1-4>` | Save or recall a scene |
| `spk auto\|off\|on` | Speaker mode. `on` forces the speaker on whatever the jack says and is not saved. |
| `audio coop\|preempt` | Run the audio thread above (default) or below the USB threads. For comparison tests: not saved, and it clears the counters. |
| `vol <-60..0>\|off` | Master volume in dB |
| `codec [cs <reg hex> \| tas <page hex> <reg hex>]` | Codec bring-up status, or read one CS42L42 or TAS2505 register |
| `charge on\|off` | Battery charging on or off |
| `dfu` | Reboot into the bootloader |
| `reboot` | Restart. The codecs are muted first. |
| `off` | Power off |
| `selftest stall\|wdt` | Reset on purpose to check the reset record. `stall` blocks the control loop and expects a stall record. `wdt` stops feeding the watchdog and expects a watchdog reset. |
| `help` | List the commands |

`audio` and `codec` exist only in the `full` build.

Each `status` line has a prefix:

| Line | Contents |
| --- | --- |
| `sp-mix` | Build variant, uptime, reset reason register, last fault (reason, PC), reset cause recorded by the firmware (`culprit`) and where it survived (`kept`), and the firmware commit (`fw`) |
| `stall` | Only after a stall reset: the control-loop stage, wait object, backtrace and peripheral registers |
| `usb` | Configured, suspended and streaming state. Packet counts by frames per packet (`n47`/`n48`/`n49`), partial packets, USB frames without a packet while streaming (`gaps`), packets handled a frame late (`late`), frame-number jumps, packets without a free buffer (`nobuf`) and receive time per packet. |
| `peak dBFS` | Input peak per channel since the previous status |
| `audio`, `audio2` | `full` build: pipeline state, buffer fill, clock ratio (`ppm`, `ff`), underruns, overruns, I2S errors and stalls, clipped samples, block time (`blk` average/max µs), and late or slow blocks and the longest gap between blocks (`gapmax`) |
| `codec` | `full` build: CS42L42 and TAS2505 bring-up result, jack register and I2C errors |
| `ctrl` | Fader volumes, balance, mute and solo masks, master dB, speaker mode/state, headphones, charging, saved scene slots, and flash writes and errors |
| `main` | Control-loop iterations, ADC scan health, and time per stage (average/max µs) |
| `threads` | Share of wall-clock time per thread. `sleep` is time with the CPU asleep. |

During playback, a healthy unit shows `under 0`, `i2s 0/0`, `gaps 0` and `nobuf 0`. `late` counts packets that were handled one frame late. Those packets are not lost.

### Reporting a problem

1. Open the web console, connect, and go to the **Status** tab. It logs one line per second and keeps the last two minutes. When the SP-1 resets, the log from before the reset is kept with the reset record.
2. Reproduce the problem if you can.
3. Click **Copy diagnostics** and paste the text into a GitHub issue. Add the host (Windows or iPadOS version, app), the firmware commit (`fw` in the first status line) and what you did. Copied text is more useful than screenshots.

## Building from source

### Workspace

The repository is itself the west manifest ([`west.yml`](west.yml)). It pulls Zephyr v4.3.1 and only the modules an nRF52840 build needs (cmsis, cmsis_6, hal_nordic).

```sh
mkdir sp1ws && cd sp1ws
git clone https://github.com/chillingartist/sp-mix
west init -l sp-mix
west update --narrow -o=--depth=1
pip install -r zephyr/scripts/requirements.txt   # Zephyr's Python packages, if not installed yet
sp-mix/scripts/apply-zephyr-patches.sh
```

### Zephyr patches

| Patch | Change |
| --- | --- |
| `0001-udc-nrf-iso-out-only.patch` | Adds `CONFIG_UDC_NRF_ISO_OUT_ONLY` to the nRF USB device driver. ISOSPLIT is set to OneDir, so the whole 1 KiB isochronous buffer serves ISO OUT (packets up to 1023 bytes), and ISO IN cannot be used. |
| `0002-uac2-adaptive-out.patch` | Adds an `adaptive` property to the UAC2 audio streaming node: the endpoint is declared adaptive, no feedback endpoint is created, and packets are sized for 49 frames (784 bytes). |

`scripts/apply-zephyr-patches.sh` applies them to `$ZEPHYR_BASE`, or else to the `zephyr` directory next to the repository. Patches that are already applied are skipped. `scripts/build.sh` runs the script before every build.

### Build

```sh
cd sp1ws/sp-mix
scripts/build.sh m0      # safety base: watchdog, recovery, power, LEDs, buttons, console
scripts/build.sh m1      # + 8-channel USB audio, received and counted but not played
scripts/build.sh full    # complete mixer (the default)
```

The output is `build-<variant>/zephyr/zephyr.bin`, which you flash through the bootloader as [above](#install-the-firmware). The release file `sp-mix-full.bin` is the `full` build. Arguments after the variant name go to `west build`. The build embeds the commit (`git describe --always --dirty`), which `status` shows as `fw`.

The application is linked at 0x20000, above the bootloader. `boards/teenage_engineering/sp1/board.cmake` also sets up J-Link, pyOCD and nrfjprog runners, but only for units with SWD access.

The settings (scenes and the last state) live in flash at 0xF7000-0xFEFFF by default. Other SP-1 firmware, feldd for one, keeps its own data at the same address, so if you switch a unit between them you may want the settings elsewhere: `SP1M_STORAGE=low scripts/build.sh full` puts them in their own 16 KiB at 0xCA000-0xCDFFF (see [`sp1-storage-low.overlay`](boards/teenage_engineering/sp1/sp1-storage-low.overlay)). The two layouts do not see each other's settings.

If you change the firmware, never remove recovery path 1 and never stop feeding the watchdog. The SP-1 has no reset button. Keep a known-good `.bin`, and bring up new builds in milestone order as described in [docs/TESTING.md](docs/TESTING.md).

### Configuration

The firmware's own Kconfig options are defined in [`Kconfig`](Kconfig). Set them in `prj.conf`, which applies to all variants. `conf/m0.conf` and `conf/m1.conf` hold the overrides for the early variants.

| Option | Default | Meaning |
| --- | --- | --- |
| `CONFIG_SP1M_VARIANT` | `"full"` | Variant name shown by `status` (set by `conf/m0.conf`, `conf/m1.conf`) |
| `CONFIG_SP1M_USB`, `CONFIG_SP1M_UAC2`, `CONFIG_SP1M_CDC`, `CONFIG_SP1M_AUDIO_OUT` | `y` | USB device, 8-channel UAC2 input, serial console, and audio output (codecs, I2S, mixer, ASRC) |
| `CONFIG_SP1M_USB_VID`, `CONFIG_SP1M_USB_PID` | `0x1209`, `0x0001` | USB IDs. The default is the [pid.codes](https://pid.codes) test ID, and distributed builds should use a registered ID. |
| `CONFIG_SP1M_USB_BCD_DEVICE` | `0x0100` | Device release number. Each variant uses its own so that Windows re-reads the descriptors. |
| `CONFIG_SP1M_HP_ATTEN_DB` | `12` | Fixed headphone attenuation below the CS42L42 maximum, in dB (0 to 62) |
| `CONFIG_SP1M_TARGET_FILL` | `240` | USB ring buffer fill set point in frames (48 frames = 1 ms) |
| `CONFIG_SP1M_I2S_BLOCKS` | `4` | I2S blocks in flight (2 to 6). Each block is 1.33 ms of margin and of latency. |

### Host unit tests

The portable core (`src/core/`) builds and runs on a PC:

```sh
cmake -S tests/host -B build-host -G Ninja
cmake --build build-host
./build-host/sp1m_tests            # or: ctest --test-dir build-host
```

The tests cover the ring buffer, the mixer (balance law, mute/solo, DC blocker, soft clip), ASRC accounting and accuracy, closed-loop simulations of the clock-rate estimator with SOF-locked and unlocked hosts, button ladder decoding and debouncing, fader conditioning, the scene storage format (CRC, out-of-range values), the UI gestures and volume safety rules, and the command parser. They are built with AddressSanitizer and UndefinedBehaviorSanitizer.

## Project layout

| Path | Contents |
| --- | --- |
| `src/core/` | Plain C without Zephyr dependencies: mixer, ASRC, clock-rate estimator, ring buffer, button decoding, fader conditioning, interaction state machine, scene storage format, command parser |
| `src/platform/` | Zephyr glue: USB, I2S, codecs (I2C), SAADC, PWM LEDs, watchdog and fault records, stall watcher, power, NVS storage, serial console |
| `boards/teenage_engineering/sp1/` | SP-1 board definition (application linked at 0x20000) |
| `app.overlay` | USB functions: UAC2 8-channel adaptive playback and CDC ACM |
| `prj.conf`, `conf/` | Full build configuration, and the `m0`/`m1` variants |
| `Kconfig` | Firmware options |
| `zephyr-patches/` | The two Zephyr patches |
| `scripts/` | `build.sh`, `apply-zephyr-patches.sh` |
| `tests/host/` | Host unit tests |
| `tools/` | Web Serial console and test WAV generators |
| `docs/` | Design document and test procedures |

## Documentation

- [docs/DESIGN.md](docs/DESIGN.md): the design document, covering design decisions, hardware constraints, architecture, the USB design, the audio pipeline, threads, safety and recovery, milestones and implementation status.
- [docs/TESTING.md](docs/TESTING.md): hardware bring-up and test procedures for each milestone.
- [CHANGELOG.md](CHANGELOG.md): release notes.

## Tools

- `tools/sp1-console.html`: Web Serial console for Chrome and Edge (see [Serial console](#serial-console)).
- `tools/channel-test.py [out.wav] [seconds] [sample rate]`: writes an 8-channel 7.1 WAV (default `sp1-channel-test.wav`, 10 s) with a 1 kHz tone on every channel, channel N at −3·N dBFS. With the host volume at 100 %, `status` should read −3, −6, ... −24 in `peak dBFS`.
- `tools/mixer-test.py [out.wav] [seconds] [sample rate]`: writes an 8-channel WAV (default `sp-mix-test.wav`, 30 s) in which each track plays its own note and rhythm at −14 dBFS peak. Track 1 plays C4 once a second, Track 2 plays E4 twice a second, Track 3 plays long G4 notes, and Track 4 plays fast C5 ticks. The pattern repeats every 3 s, so it loops cleanly. With it, fader, mute, solo and balance changes are easy to hear.

Both generators use only the Python 3 standard library.

## Credits and references

**Acknowledgements:** TKT for [solderless.engineering](https://solderless.engineering), which makes flashing and testing custom firmware possible; Tim Knapen for [SP-1-dev](https://github.com/timknapen/SP-1-dev), the hardware documentation (pin map, codec and clock notes, bootloader) this firmware is built on; chattock (Technics) and marc ([marcabisamra](https://github.com/marcabisamra)) for the [tape looper firmware](https://github.com/chattock/sp1-tape-looper), whose work this builds on; and Benjamin Reece for [feldd](https://github.com/bnjreece/feldd-sp1-firmware), whose watchdog and power handling informed this one.

- [Zephyr Project](https://zephyrproject.org): the RTOS and USB device stack this firmware is built on.
- [SP-1-dev](https://github.com/timknapen/SP-1-dev) by **Tim Knapen** (MIT): SP-1 hardware documentation, including pins, I2S and I2C, the CS42L42 initialization sequence and the bootloader constraints.
- [sp1-tape-looper](https://github.com/chattock/sp1-tape-looper) by **chattock** (MIT): a Zephyr and UAC2 firmware for the SP-1. The measured button ladder levels, the TAS2505 clock setup, and the power-off and recovery approach come from it.
- [feldd-sp1-firmware](https://github.com/bnjreece/feldd-sp1-firmware) by **Benjamin Reece** (MIT): watchdog handling, power-up policy and the TRS jack pins.

Their copyright and license notices are reproduced in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- [Solderless web updater](https://solderless.engineering): web tool for flashing the SP-1 through its bootloader.
- Zephyr v4.3.1 sources patched here: [udc_nrf.c](https://github.com/zephyrproject-rtos/zephyr/blob/v4.3.1/drivers/usb/udc/udc_nrf.c) and [usbd_uac2_macros.h](https://github.com/zephyrproject-rtos/zephyr/blob/v4.3.1/subsys/usb/device_next/class/usbd_uac2_macros.h).
- [Microsoft: USB Audio 2.0 drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers).
- [TI TAS2505 Application Reference Guide (SLAU472C)](https://www.ti.com/lit/ug/slau472c/slau472c.pdf) and the Cirrus Logic CS42L42 datasheet (DS1083F8).

## License

Copyright (c) 2026 chillingartist. This project is released under the MIT License (see [LICENSE](LICENSE)), except for `zephyr-patches/`. Those files modify Zephyr and remain under Zephyr's Apache License 2.0. Material from SP-1-dev (Tim Knapen), sp1-tape-looper (chattock) and feldd-sp1-firmware (Benjamin Reece), all MIT licensed, is credited with their notices in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

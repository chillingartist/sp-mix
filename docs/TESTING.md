# SP-Mix — Hardware bring-up and testing

This guide takes an SP-1 through the three firmware build variants in order: `m0` (safety base), `m1` (USB probe) and `full` (the mixer). Each stage starts by making sure you can get back to the bootloader. It also explains the diagnostics the firmware reports and how to report results. The design behind the firmware is described in [DESIGN.md](DESIGN.md).

> **Read this first.** SP-Mix is unofficial firmware. It is not affiliated with or endorsed by teenage engineering; SP-1 is a trademark of teenage engineering. Flashing third-party firmware is at your own risk: keep a firmware image that you know works on your SP-1, and learn the recovery paths in section 2 before you flash anything. Protect your hearing: the firmware limits the master volume at power-up and when headphones are plugged in, but start with the volume low anyway, and keep headphones off your ears until the headphone checks in M2 have passed.

## 1. Preparation

**You need**

- An SP-1 and a USB cable.
- A Windows 10 (1703 or later) PC and/or an iPad. The console page needs a desktop browser with Web Serial (Chrome or Edge), so a computer is needed for the diagnostics even when you test with an iPad.
- Firmware builds. Set up the west workspace as described in the README, then:

  ```sh
  scripts/build.sh m0      # safety base
  scripts/build.sh m1      # + 8-channel USB probe (counts audio, plays nothing)
  scripts/build.sh full    # the mixer
  ```

  Each build writes `build-<variant>/zephyr/zephyr.bin`. The next build of the same variant overwrites it, so copy every binary that works to a safe place before you build again.
- The update tool for the SP-1's bootloader. `zephyr.bin` is flashed through the bootloader with the Solderless web updater (<https://solderless.engineering>), as described in the README.
- The console page `tools/sp1-console.html` (section 3).
- Test signals generated with `tools/channel-test.py` and `tools/mixer-test.py` (section 4).
- Headphones. For M2, a player that can loop an 8-channel WAV file with the SP-1 as its output device. For multichannel work: on the iPad an app with per-channel routing such as AUM; on Windows a DAW with multichannel WASAPI output such as Reaper, or FlexASIO.

**Ground rules**

- Work through the stages in order, and keep the last binary that worked at every stage.
- When a step does not give the expected result, stop there, copy the diagnostics (section 9) and report it (section 10). Do not carry on to the next step.
- Copy the diagnostics at the checkpoints even when everything looks fine. CPU headroom, clock tracking and I2C error counts cannot be seen or heard; they are only in the record, and normal values are the baseline for later comparisons.
- The first line of `status` names the build: `sp-mix <variant> ... fw <hash>` (`sp1mixer` in v0.1.0). Check that it is the build you just flashed. `<hash>` is the `git describe` of the source tree; a `-dirty` suffix means the tree had uncommitted changes.

## 2. Recovery paths (learn these first)

The SP-1 has no reset button. The bootloader starts a watchdog (about 5 s) that the firmware must keep feeding; if it stops, the chip resets. There are three ways back to the bootloader. All three have been verified on hardware.

1. **Hold Track 1 + Track 4 for 3 s while the firmware runs.** The four track LEDs light up gradually; after 3 s the SP-1 resets into the bootloader. Keep holding both keys until the updater sees the SP-1. This works in every mode.
2. **Hold Track 1 + Track 4 while plugging in USB.** This is the bootloader's own path and works whatever firmware is installed. It is the fallback when nothing else works.
3. **The `dfu` console command.** Reboots straight into update mode without any key. The firmware answers `rebooting to the bootloader; if the updater does not see the SP-1, hold Track 1 + Track 4 while replugging USB`.

If you change the firmware: never remove path 1, and never stop the firmware from feeding the watchdog. If you can, try risky changes on a unit with a debugger attached first.

**Power**: hold FUNCTION alone (no other key) for 3 s to power off; the playback LEDs count down. With USB connected, the SP-1 goes into a standby that only shows charging; hold FUNCTION for 0.6 s to power it on again. When no host has configured the USB device and nothing has been touched for 10 minutes, the SP-1 powers off.

## 3. The console

Connected over USB, the SP-1 also appears as a serial port (any baud rate). Close every other program that has the port open before you connect.

**Web console page**: download `tools/sp1-console.html`, open it in Chrome or Edge, connect, and choose the SP-1's serial port. The page has three tabs:

- Buttons and faders: live ladder and fader readings, and a guided check that asks you to press each key in turn and then generates a text report.
- Status: sends `status` once a second and shows the device, the reset history, a one-line-per-second history of the last 2 minutes, CPU use per thread, USB counters, per-channel peaks, and the audio, codec and mixer state. When the device resets, the history before the reset is frozen in that reset's entry. A button copies the whole diagnostics record (reset entries with their stall lines, the recent history and the latest raw `status` output) to the clipboard as text. If the browser does not allow clipboard access, the text is placed in the terminal tab instead.
- Terminal: type commands and see the raw replies.

**Any serial terminal**: commands can also be typed into a terminal program such as PuTTY. The firmware does not echo input, so turn on local echo. `watch on` prints `status` every second, which is useful for long logs.

**Commands** (as listed by `help`):

| Command | Effect |
| --- | --- |
| `status` | Status report (section 9) |
| `watch on\|off` | `status` every second |
| `adc` | Raw ladder, fader and battery ADC readings and the decoded keys |
| `threads` | CPU % and free stack per thread |
| `clear` | Reset the USB and audio counters |
| `scene save\|load <1-4>` | Save / recall a scene |
| `spk auto\|off\|on` | Speaker: on when no headphones are plugged in / always off / forced on (`on` is not saved and ignores the jack; for testing) |
| `audio coop\|preempt` | Full build only. Audio thread above (default) or below the USB threads; not saved; clears the counters |
| `vol <-60..0>\|off` | Master volume, dB |
| `codec [cs <reg hex> \| tas <page hex> <reg hex>]` | Full build only. Codec status, or read one register, e.g. `codec cs 1b77`, `codec tas 0 3f` |
| `charge on\|off` | Battery charging on / off (for hosts with a tight power budget) |
| `dfu` | Reboot into the bootloader |
| `reboot` | Reboot |
| `off` | Power off |
| `selftest stall\|wdt` | Reset on purpose to check the reset record: `stall` stops the control loop (expect a stall record, stage 7); `wdt` stops feeding the watchdog (expect culprit `0x4a`) |
| `help` | List the commands |

## 4. Test signals and channel mapping

The SP-1 enumerates 8 channels in a 7.1 layout:

| Channels | Windows position | Track |
| --- | --- | --- |
| 1–2 | Front left / front right (FL/FR) | Track 1 |
| 3–4 | Center / LFE (FC/LFE) | Track 2 |
| 5–6 | Back left / back right (BL/BR) | Track 3 |
| 7–8 | Side left / side right (SL/SR) | Track 4 |

Both generators write a 16-bit WAVE_FORMAT_EXTENSIBLE file with channel mask 0x63F (FL FR FC LFE BL BR SL SR, the order the device declares), and need only Python 3:

```sh
python3 tools/channel-test.py [out.wav] [seconds] [sample rate]   # default sp1-channel-test.wav, 10 s, 48000 Hz
python3 tools/mixer-test.py   [out.wav] [seconds] [sample rate]   # default sp-mix-test.wav, 30 s, 48000 Hz
```

**`channel-test.py`** (routing and levels): every channel plays a 1 kHz tone at the same time, channel N at −3·N dBFS (channel 1 at −3 dB … channel 8 at −24 dB). With the host volume at 100%, the `peak dBFS` line of `status` should read `-3 -6 -9 -12 -15 -18 -21 -24`. Channel 4 is the LFE position: if the host low-passed it, it would read far below −12. A low sample rate keeps long files small (the host resamples to 48 kHz and the 1 kHz peaks stay the same), e.g. `python3 tools/channel-test.py long.wav 1800 8000` for 30 minutes (about 230 MB); looping a short file in the player works too.

**`mixer-test.py`** (controls): each track plays its own note in its own rhythm, the same on left and right, so a faded, muted, soloed or panned track is easy to hear and the track LEDs move:

| Track | Note | Rhythm |
| --- | --- | --- |
| 1 | C4 (low C) | once a second |
| 2 | E4 | twice a second |
| 3 | G4 | long notes |
| 4 | C5 (high C) | fast ticks |

Each track peaks at −14 dBFS, so all four together stay below the soft clipper at master 0 dB. The length is rounded to whole 3 s cycles, so the file loops cleanly; play it on repeat.

**Host notes**

- Windows shared mode: the system volume slider is a software volume in front of the device. Keep it at 100% for level checks (the M2 steps below say when to lower it).
- Windows: if the SP-1 re-enumerates (reflash, reboot, `selftest`, replug) while a player is open, the player's existing stream is moved back as stereo and only Track 1 (channels 1–2) plays. Stop and start playback again, select the SP-1 explicitly as the output device, or use WASAPI exclusive mode or ASIO through FlexASIO. iPad apps reconfigure automatically.
- Multichannel output needs software that routes per channel: on the iPad for example AUM, on Windows for example Reaper (WASAPI) or FlexASIO.

## 5. M0: safety base (`scripts/build.sh m0`)

The `m0` build contains the watchdog, the recovery paths, power handling, LEDs, buttons, faders and the serial console. It has no USB audio, and the codecs are held in reset.

1. **Flash** `m0`. Expected: the LEDs light up fully, then dim; the track LEDs stay faintly lit. Tap a track key: the track is muted and its LED goes dark; tap again to unmute.
2. **Buttons and faders**: on the console page's buttons-and-faders tab, run the guided per-key check and move every fader to both ends. Alternatively, compare `adc` readings with the band table in `src/core/buttons.c`. Expected: every key and combination decodes to the right key; the raw fader range covers 60–3600 (one SP-1 measured 0 at the bottom and 3700–3820 at the top). If it does not, adjust `FADER_RAW_LO` / `FADER_RAW_HI` in `src/core/fader.h` and report the readings.
3. **Recovery paths**: try paths 1, 2 and 3 from section 2 once each, and flash `m0` again after each.
4. **Power**: hold FUNCTION for 3 s to power off; unplug USB; FUNCTION powers the SP-1 on again. With USB connected, power-off leads to standby, and holding FUNCTION for 0.6 s powers it on.
5. **No unexpected resets**: leave it running for more than a minute, then check `status`: `up` keeps increasing between readings, `fault none`, `culprit 0x00`, `reset` does not have the watchdog bit (0x00000002) set; in the `main` line `loops` keeps increasing, `adc ok` keeps increasing and `nostart`, `timeout` and `short` stay at 0.
6. **Reset records** (optional): `selftest wdt` should end in a watchdog reset, after which `status` shows `culprit 0x4a kept ram` (a watchdog reset clears the register copy; the RAM copy survives). `selftest stall` should end in a soft reset, after which `status` shows `culprit 0x67 kept reg+ram` and three `stall` lines with `stage 7` (console commands).

**Stop** if a recovery path does not work (get back with path 2 and do not continue), if the SP-1 resets on its own, or if a key decodes wrongly.

## 6. M1: USB probe (`scripts/build.sh m1`)

The `m1` build adds the adaptive 8-channel UAC2 interface. Received audio is counted (packet sizes, gaps, per-channel peaks) but not played: there is no sound in this stage.

1. **Enumeration**: Windows and the iPad each see an 8-channel playback device named "SP-Mix" ("SP-1 Mixer" in v0.1.0), plus a serial port.
2. **Routing and levels**: play the `channel-test.py` file with the host volume at 100%. Expected: `peak dBFS -3 -6 -9 -12 -15 -18 -21 -24`, in this order. If only channels 1–2 show a level, see the Windows note in section 4. If channel 4 reads far below −12, the host is filtering the LFE channel; report it.
3. **Packet counters**: in the `usb` line, `stream 1` while playing; `n48` keeps growing (Windows sends 48-frame packets); `part`, `gaps` and `nobuf` stay at 0. `late` (packets handled one USB frame late) is not a loss, and `jumps` counts large steps such as host pauses.
4. **30-minute run**: `clear`, then play continuously for 30 minutes with `watch on` in a terminal or with the console page's status tab open; do not let the computer sleep. Expected: `n48` keeps growing, `gaps`, `part` and `nobuf` stay at 0, and there is no new reset entry.

**Results so far**: Windows and the iPad both enumerate 8 channels. On Windows the channel test tones matched on every channel (−3 … −24 dBFS, including channel 4), all packets were 48 frames, `gaps`, `part` and `nobuf` stayed at 0, and the CPU was about 26% busy while streaming. The 30-minute run has not been done yet.

**Stop** if the SP-1 does not enumerate as 8 channels, if `gaps` or `nobuf` grow while playing, or if it resets.

## 7. M2: audio output, first full run (`scripts/build.sh full`)

Copy the diagnostics record at the four checkpoints below (records A–D) and paste each into a text file. If a step goes wrong, stop and copy the record at that moment.

**Prepare**: the `full` binary; the console page; `sp-mix-test.wav` from `tools/mixer-test.py` set to loop in the player; a working `m1` binary to fall back to. Do the iPad after everything below has passed on Windows.

0. **Before flashing (still on `m1`)**: pull all four faders to the bottom and wait 10 s (the last state is saved once nothing has changed for 5 s); unplug the headphones; set the Windows volume to 10%. Hold Track 1 + Track 4 for 3 s to enter the bootloader and flash `full`.
1. **Start-up check**: the SP-1 restarts after flashing. Open the console page, connect, go to the status tab and wait 5 s.
   - `sp-mix full ... culprit 0x00 ... fw <hash>`.
   - `audio idle`; the page shows no warning for the audio state, and there is no `audioerr` line.
   - `ctrl ... master -20` or `-24` (never above −20), `spk auto/1`, `hp 0`.
   - In the terminal, `codec` replies `codec cs42 ok step 0 tries 1 osc 0x02 tas ok ... i2cerr 0`, and the value after `jack` is below `0x80`. `tries 2` also works; note it in your report.
   - Copy the diagnostics: **record A**. Stop if the audio did not start (`audio2 run 0`, or `init` not 0), if `cs42` or `tas` is not `ok`, if `osc` is not `0x02`, or if the I2S stall count (second number of `i2s`) keeps growing, which means there is no LRCLK.
2. **Stall record self-test**: type `selftest stall`. The SP-1 mutes and restarts by itself after a second or two; the page reconnects (if it does not, connect again). The reset history gains an entry stuck in console commands: `culprit 0x67`, `stall src ... stage 7`. Copy the diagnostics: **record B**.
3. **Speaker**: if a track LED blinks slowly, that fader is waiting to take over a stored volume; push it to the top and back to the bottom. Then start `sp-mix-test.wav` on repeat. With all faders at the bottom there should be no sound.
   - `audio run`, and all 8 `peak dBFS` values around −14. If only channels 1 and 2 show a level, stop playback and start it again (see the Windows note in section 4).
   - Push faders 1–4 to the middle one after the other: low C, E, G and high C join in one by one and form a chord.
   - Set the Windows volume to 100% and adjust the level with VOL+/− (1 dB per press, hold to repeat; the playback LEDs show the master volume while you press).
   - Expected: `under 0`, `i2s 0/0`, `blk` average below 700 µs (around 500 is expected) and maximum below 1000 µs.
4. **Headphones** (safety check: hold the headphones in your hand, do not wear them yet)
   - With the headphones unplugged, type `vol -10`: the speaker gets louder.
   - Plug the headphones in: within half a second `hp 1`, the speaker stops, and the master volume drops to −20 at once. Holding the headphones close, you hear the chord quietly.
   - Unplug them: `hp 0`, the speaker resumes within a fraction of a second, and the master volume stays at −20 (it does not go back up by itself).
   - Repeat 5 times. Stop and report if the speaker keeps playing with headphones plugged in, if `hp` does not change, or if the master volume is not lowered.
   - When all is well: type `vol -40`, put the headphones on, and raise the volume slowly with VOL+.
5. **Controls** (balance needs headphones; everything else works with either output)
   - Faders: pulling a fader down makes its note quieter at once, and at the bottom it is gone.
   - Mute: tap a track key (release within 0.6 s; it acts on release). The note stops and the track LED goes dark; tap again to bring it back. Try all four.
   - Solo: hold FUNCTION and tap a track key. Only that note remains, and its LED blinks fast. Another track can be soloed as well; FUNCTION + the same key again cancels. Solo overrides mute.
   - Balance: hold the Track 1 key and move fader 1 (it starts after a short movement). The low C moves to one side; about a third of the fader travel takes it all the way. Releasing the key does not mute. The Track 1 LED now blinks slowly: the fader is at a balance position and takes over the volume again only after passing the stored volume position (pulling it down takes over immediately). Bring the balance back to the centre the same way.
   - Scenes: set up a state (for example Track 2 muted, fader 3 in the middle). Tap PLAY: the playback LEDs start to pulse (scene layer). Hold FUNCTION and tap Track 1 to save scene 1; the Track 1 LED blinks to confirm. After 3 s without input the scene layer closes. Change things (unmute Track 2, fader 3 to the bottom), then tap PLAY and tap Track 1 to recall: Track 2 is muted again and Track 3 plays at the middle volume again. The LEDs of tracks whose fader does not match the scene blink slowly until the fader passes the stored position. A recall never raises the master volume.
   - LEDs: the track LEDs pulse with each track's rhythm (Track 4 fastest). They show the level the host sends, so pulling a fader down does not dim them; muted tracks are dark and soloed tracks blink fast. In the default meter mode the four playback LEDs show the output level: fewer light up as faders or the master volume go down, and they go dark within a second after playback stops. Tap FWD or BACK to switch the playback LEDs to status mode (USB connected, playing, headphones plugged in, charging) and back.
   - Speaker mode: unplug the headphones, hold FUNCTION and tap FWD or BACK: `spk off/0` and the speaker stops. Do it again to return to `auto`.
   - Copy the diagnostics: **record C**.
6. **30 minutes**: type `clear`, play music or the looped test file, keep the console page on the status tab, and do not let the computer sleep. After 30 minutes: `under 0`, `i2s 0/0`, no new reset entry, `ppm` stable (not drifting in one direction), and no `?` after the `ff` value. Copy the diagnostics: **record D**.
7. **Way back (in `full`)**: hold Track 1 + Track 4; the track LEDs ramp up and after 3 s the SP-1 is in the bootloader, where the updater sees it. Flash `full` again. Then type `dfu`: it should also enter the bootloader. Flash `full` again.

Useful commands while debugging: `codec` (chip status), `codec cs 1b77` (read the headphone detection register), `spk on` (force the speaker on whatever the jack says; not saved), `vol <dB>`.

**Results so far**: the CS42L42 and TAS2505 came up on the first attempt, the speaker plays (L+R)/2, headphone detection switches the speaker off and caps the master volume at −20 dB on insertion, and faders, mute, solo, balance, scenes and the LED meters have been tested. The 30-minute run (step 6) and scene retention across power-off have not been tested yet.

## 8. Regression test (after every new full build)

A short run that checks the real-time behaviour of a new build. It needs one record.

1. Flash the new `full`, open the console page on the status tab.
2. Play for about 5 minutes (your own music or a test file) without touching anything.
3. Stop playback, wait 3 s, then play for 1 more minute.
4. Copy the diagnostics.

Expected:

- The first status line shows `fw <hash>` of the build you flashed.
- `i2s 0/0` and `under 0` in the `audio` line.
- `blk` (blocks that played audio) average about 450–550 µs, maximum below 1333 µs.
- `audio2 ... slow 0` (no block took longer than its 1333 µs period).
- `gapmax` below 2000 µs (normally about 1333).
- `usb ... gaps 0 ... nobuf 0`. `late` (packets handled one frame late) is not a loss.
- `sofrej` close to 0.
- Headphones and speaker both sound normal.

`audio preempt` in the terminal switches back to the old audio thread priority for comparison (it clears the counters, is not saved, and a reboot returns to `coop`).

**Reference result** (second full run, Windows, about 12 minutes with one stop and restart): 0 I2S errors, 0 underruns, playing blocks 495 µs on average and 761 µs at most, longest gap between blocks 1.63 ms, audio thread 38%, usbd 11%, total CPU busy 63%, USB 48 µs per packet.

## 9. Reading the diagnostics

`status` prints the following lines. The `audio`, `audio2` and `codec` lines exist only in the `full` build; the `stall` lines appear only when the previous reset was caused by the stall catcher; `audioerr` appears only after an I2S error.

```
sp-mix <variant> up <s>s reset 0x<resetreas> fault none|yes [reason <n> pc 0x<addr>] culprit 0x<code> [kept reg|ram|reg+ram] fw <hash>
stall src sof|timer stage <n> up <ms> loops <n> state 0x<bits> run <0|1> wait 0x<addr> to <ms> tmr <n> pc 0x<addr> lr 0x<addr>
stall bt 0x<addr> ... (8 addresses)
stall hw saadc 0x<..> inten 0x<..> iser 0x<..> 0x<..> ispr 0x<..> 0x<..> rtc <n> cc0 <n> inten 0x<..> ppi 0x<..>
usb cfg <0|1> susp <0|1> stream <0|1> pkts <n> n47 <n> n48 <n> n49 <n> other <n> zlp <n> part <n> gaps <n> late <n> jumps <n> nobuf <n> rx <avg>/<max>
peak dBFS <ch1> <ch2> <ch3> <ch4> <ch5> <ch6> <ch7> <ch8>
audio idle|preroll|run|fade fill <n> lin <x> ppm <x> ff <x>[?] under <n> over <n> drop <n> i2s <errors>/<stalls> sofrej <n> clip <n> blk <avg>/<max> us
audio2 run <0|1> init <err> blocks <n> restartfail <n> last <err> prio coop|preempt playing <n> slow <n> late <n>/<n>/<n> gapmax <us> sofrel <n>
audioerr kind late|stall|start at <s.ms> state <state> fill <n> gap <us> us <us> code <err>
codec cs42 ok|partial|none step <n> tries <n> osc 0x<..> tas ok|fail step <n> spk <0|1> jack [?]0x<..> i2cerr <n>
ctrl vol <t1> <t2> <t3> <t4> bal <t1> <t2> <t3> <t4> mute 0x<m> solo 0x<m> master <dB> spk auto|off|on/<0|1> hp 1|0|?|- chg <0|1>[ (off)] slots 0x<m> dirty <0|1> writes <n>/<n> err
main loops <n> stage <n> tmr <n> adc ok <n> nostart <n> timeout <n> short <n> busy <n> pcycle <n>
main us wait <avg>/<max> in <avg>/<max> out <avg>/<max> ui ... mix ... leds ... cmds ... persist ... status ...
threads <name> <pct> <name> <pct> ... sleep <pct>
```

The USB and audio counters run from boot or from the last `clear` (or `audio coop|preempt`); the peaks, `rx`, `main us` and `threads` cover the time since the previous `status`.

### First line: build and reset

| Field | Meaning |
| --- | --- |
| `sp-mix <variant>` | Build variant: `m0`, `m1` or `full` |
| `up` | Seconds since boot. If it goes down between readings, the SP-1 has reset |
| `reset` | RESETREAS of this boot: 0x1 reset pin, 0x2 watchdog, 0x4 soft reset, 0x8 CPU lockup, 0x10000 woken from power-off (FUNCTION), 0x100000 woken by USB power; 0 is a power-on reset |
| `fault` | `yes` if the previous run ended in a fatal error; `reason` and `pc` say which and where |
| `culprit` | Code left behind by the previous run (table below); 0x00 = none |
| `kept` | Where the culprit survived: `reg` (GPREGRET2), `ram` (`__noinit` RAM) or both. A soft reset keeps both, a watchdog reset only RAM |
| `fw` | Build identifier (`git describe`; `-dirty` = uncommitted changes) |

| Culprit | Meaning |
| --- | --- |
| `0x41` | The audio thread stopped (watchdog no longer fed) |
| `0x49` | No SAADC scan of buttons and faders for 1 s; the firmware reset once |
| `0x4a` | `selftest wdt`: watchdog feeding stopped on purpose |
| `0x50` + n | Watchdog timeout while thread n held the CPU: 1 main, 2 idle, 3 sysworkq, 4 usbd, 5 udc_nrfx, 6 console, 7 store, 8 audio, 0 other |
| `0x60` + stage | The stall catcher found the control loop stuck in that stage (below) |
| `0x70` + stage | The same, and the kernel timer had stopped as well |
| `0x80` + irq | Watchdog timeout inside interrupt `irq` (0–63) |
| `0xf0` + r | Fatal error (reason & 0x0f) |

Control loop stages (used by `culprit`, `stall ... stage` and `main ... stage`): 1 `wait` (for the next 2 ms tick), 2 `in` (ADC scan), 3 `out` (headphone poll, audio levels), 4 `ui`, 5 `mix` (mix parameters, speaker switch), 6 `leds`, 7 `cmds` (console commands), 8 `persist`, 9 `status` (status snapshot, watchdog supervision), 10 settle (fader settling before the loop starts), 11 flush (synchronous scene save), 12 standby, 13 off.

### Stall record

| Field | Meaning |
| --- | --- |
| `src` | Which watcher noticed: `sof` (USB SOF callback) or `timer` (kernel timer) |
| `stage`, `up`, `loops` | Control loop stage, uptime in ms and loop count when it got stuck |
| `state`, `run` | Main thread state bits and whether it was running. The console page decodes it: bit 0x02 = waiting on something, 0x04 = sleeping, 0 = not waiting (the CPU was taken by other threads or interrupts) |
| `wait`, `to` | Wait queue the main thread was pending on, and its remaining timeout in ms (−1: none) |
| `tmr` | Kernel timer beats while stuck |
| `pc`, `lr`, `bt` | Code addresses of the main thread. Look them up in the matching `build-<variant>/zephyr/zephyr.elf`, for example with `arm-zephyr-eabi-addr2line -e build-full/zephyr/zephyr.elf <addr>` |
| `hw` | SAADC, NVIC, RTC1 and PPI registers at that moment |

The record is repeated in every `status` until the next reset; the console page keeps one copy with the reset entry. Always include it in a report.

### USB

| Field | Meaning | Expected while playing |
| --- | --- | --- |
| `cfg`, `susp`, `stream` | Configured by the host, suspended, host streaming (Alt 1) | `1 0 1` |
| `pkts` | Packets received | Growing |
| `n47`, `n48`, `n49`, `other` | Packets by size in frames | Windows: all `n48` |
| `zlp` | Zero-length packets (the host keeps the stream open but sends nothing) | |
| `part` | Packets that are not a whole number of frames | 0 |
| `gaps` | USB frames without a packet while streaming: lost packets | 0 |
| `late` | Packets handled one USB frame late (the audio thread runs above the USB thread); not a loss | Small |
| `jumps` | Frame number steps of more than 16 frames, e.g. host pauses; counted apart from `gaps` | Occasional, at stop/start |
| `nobuf` | No receive buffer free for a packet | 0 |
| `rx` | Average/longest time per received packet, µs | |

`peak dBFS`: per-channel input peaks since the previous `status`, as sent by the host (before the faders). `-99` means silence.

### Audio (full build)

| Field | Meaning | Expected |
| --- | --- | --- |
| state | `idle` (no stream), `preroll` (buffering up to the target), `run`, `fade` (fading out after the host stopped) | `run` while playing |
| `fill` | Ring buffer fill in frames (48 frames = 1 ms) | |
| `lin` | Linearised fill seen by the rate loop; it is regulated to `CONFIG_SP1M_TARGET_FILL` (240 by default) | Near 240 |
| `ppm` | Ratio offset applied by the ASRC | Stable, not drifting in one direction |
| `ff` | Feed-forward clock ratio estimate; a trailing `?` means it is not valid yet (it needs about 1 s of consistent SOF timestamps) | No `?` while playing |
| `under` | Underruns: a block needed more frames than the ring buffer held | 0 |
| `over` | The ring buffer ran full and was cut back | 0 |
| `drop` | Received frames that did not fit into the ring buffer | 0 |
| `i2s` | I2S errors / stalls. An error is a block that came too late (the output ran dry) or a failed start; a stall means no free block for 20 ms, i.e. no LRCLK from the CS42L42 | `0/0` |
| `sofrej` | SOF timestamps that did not fit the expected timing (the feed-forward estimate restarts) | Close to 0 |
| `clip` | Output samples that reached the soft clipper | 0 unless driven hard |
| `blk` | Average/longest processing time of blocks that played audio, µs; one block period is 1333 µs | About 500 / below 1333 |

| `audio2` field | Meaning | Expected |
| --- | --- | --- |
| `run`, `init` | The audio thread exists; result of the audio set-up (0 = OK) | `1`, `0` |
| `blocks` | Blocks processed | Growing |
| `restartfail`, `last` | I2S restarts that failed; last I2S error code | 0 |
| `prio` | `coop` (audio thread above the USB threads, default) or `preempt` | `coop` |
| `playing` | Blocks that played audio | |
| `slow` | Playing blocks that took longer than one block period | 0 |
| `late` | Blocks that started 1 / 2 / 3+ periods late | `0/0/0` |
| `gapmax` | Longest time between two block starts, µs. Normally 1333; with 4 I2S blocks in flight, about 5.3 ms runs the output dry | Below 2000 |
| `sofrel` | SOF timestamps relabelled from their spacing (a frame number read right at a SOF); harmless corrections | Small |

`audioerr` describes the last I2S error: `kind` (`late`: block too late, output ran dry; `stall`: no LRCLK; `start`: I2S did not start), uptime (`at`), audio state, ring fill, time since the previous block start (`gap`), duration of the failing block (`us`) and the driver error code.

### Codecs (full build)

| Field | Meaning | Expected |
| --- | --- | --- |
| `cs42` | CS42L42 (headphones): `ok` fully set up, `partial` answered on I2C but set-up failed, `none` no answer | `ok` |
| `step` (after `cs42`) | Failed CS42L42 step: 1 clocks, 2 serial port, 3 headphone path; 0 none | 0 |
| `tries` | CS42L42 set-up attempts (a second one with a longer reset after a failure) | 1 (2 works, but report it) |
| `osc` | Register 0x1109: 0x02 once the chip runs from SCLK with its internal RC oscillator off | `0x02` |
| `tas`, `step` | TAS2505 (speaker): `ok` or `fail`, and the failed write (1-based; 0 none) | `ok`, 0 |
| `spk` | What the speaker amplifier was last set to | |
| `jack` | Last reading of register 0x1B77; bit 0x80 = headphones plugged in; a `?` before the value means no valid reading yet | Below 0x80 without headphones |
| `i2cerr` | I2C errors | 0 |

### Mixer state

| Field | Meaning |
| --- | --- |
| `vol` | Track volumes, 0–1023 |
| `bal` | Track balance, −100 (left) to 100 (right) |
| `mute`, `solo` | Bit masks: Track 1 = 0x1, Track 2 = 0x2, Track 3 = 0x4, Track 4 = 0x8 |
| `master` | Master volume in dB (−61 = off) |
| `spk` | Speaker mode (`auto`, `off`, or `on` when forced with `spk on`) / speaker currently on (1/0) |
| `hp` | Headphones plugged in: `1`, `0`, `?` not known yet, `-` build without audio output |
| `chg` | Charging (1/0); `(off)` = charging switched off with `charge off` |
| `slots` | Bit mask of scene slots that hold a scene |
| `dirty` | Settings changed but not yet written to flash (writes wait until playback stops) |
| `writes` | Flash writes / errors |

### Control loop and CPU

| Field | Meaning |
| --- | --- |
| `main loops` | Control loop iterations (500 per second) |
| `stage`, `tmr` | Current stage; stall-catcher timer beats |
| `adc ok` | Completed SAADC scans |
| `nostart`, `timeout`, `short` | Failed scans: the start did not take, the scan never ended, too few results. Should stay at 0 |
| `busy` | Calls made while a scan was still running |
| `pcycle` | SAADC power cycles after repeated failures |
| `main us` | Average/longest µs per loop iteration in each stage (M1 measured about 55–65 µs per iteration in total) |
| `threads` | Share of wall-clock time per thread in %, and `sleep` (CPU halted). The console page's "CPU busy" is 100 − idle − sleep |

## 10. Reporting results

1. On the console page's status tab, copy the diagnostics record. It is plain text: paste it as text, not as a screenshot.
2. Open an issue in this project's GitHub repository and attach the copied diagnostics. Include:
   - the build (`sp-mix <variant> ... fw <hash>`, and whether you changed any configuration),
   - the host (Windows version or iPadOS version, the player or app, and for Windows shared, exclusive or ASIO mode),
   - the stage and step from this guide, what you expected and what happened,
   - for a stage run with several checkpoints (records A–D in M2), all records in one issue.
3. For button or fader problems, also attach the report generated by the per-key check on the buttons-and-faders tab.
4. If the SP-1 reset unexpectedly, the reset entry and its `stall` lines are part of the copied record; make sure they are included.

## Host unit tests

The portable core (`src/core/`) has unit tests that run on a PC: ASRC accuracy, closed-loop simulation of the rate loop with SOF-locked and unlocked hosts, button ladder decoding, every gesture of the interaction state machine, and the CRC and range checks of the scene storage format.

```sh
cmake -S tests/host -B build-host -G Ninja
cmake --build build-host && ./build-host/sp1m_tests
```

# SP-Mix — Design

This document describes the design of the SP-Mix firmware. It is a revision of the original plan (2026-10-13). The goals, the scope and the central idea of the original plan are unchanged: 8 channels in, an adaptive USB endpoint without a feedback endpoint, and an asynchronous sample rate converter (ASRC) on the device. This revision corrects the points where the original plan did not match Zephyr or the SP-1 hardware, and reorders the milestones by risk. Section 11 lists the changes from the original plan; section 12 records what has been verified on hardware so far.

The hardware bring-up and test procedure is in [TESTING.md](TESTING.md).

SP-Mix is an unofficial project. It is not affiliated with or endorsed by teenage engineering. SP-1 is a trademark of teenage engineering.

## 1. Goals and scope

Version 1 turns the SP-1 into a class-compliant USB 8-channel (4 stereo tracks) hardware mixer: an iPad or a Windows PC sends 4 stereo tracks, and the SP-1 mixes them to stereo in real time with its faders and buttons and plays the result on the headphone jack or the speaker.

**In scope for v1**

- USB Audio Class 2.0 playback device, 48 kHz / 16 bit / 8 channels, adaptive synchronisation, no driver needed
- Per track: volume (fader), mute, solo, balance (stereo pan); global: master volume
- Headphone output (CS42L42) and speaker output (TAS2505), switching automatically when headphones are plugged in
- Scene snapshots: 4 slots, kept across power-off
- USB CDC serial port: diagnostic counters, configuration, reboot into the bootloader

**Out of scope for v1**: MIDI, audio back to the host, the 3.5 mm analog input, Android, Bluetooth, eMMC, effects (apart from a safety soft clipper).

| Item | Decision |
| --- | --- |
| Host platforms | iPadOS; Windows 10 1703 and later |
| Audio format | 48 kHz, 16 bit, 8 channels |
| USB synchronisation | Adaptive OUT, no feedback endpoint (confirmed to work on the iPad; listed as supported in the documentation of the Windows usbaudio2.sys driver) |
| Software stack | Zephyr v4.3.1, new USB device stack (device_next), with two patches kept in this repository |
| Persistence | Scene snapshots in the nRF52840 internal flash (NVS), written only while not playing |

## 2. Hardware constraints and architecture decisions

**The core decision is unchanged**: the USB endpoint is adaptive without a feedback endpoint, and a software ASRC on the device converts from the USB clock domain to the clock domain of the on-board oscillator.

1. The nRF52840 has a single pair of isochronous endpoints that share a 1 KB buffer. With ISOSPLIT=OneDir the whole buffer goes to one direction, with a limit of 1023 bytes per packet; split in half, each direction gets about 512 bytes.
2. 8 channels need 48 frames × 8 × 2 B = 768 B per millisecond, or 784 B when the host adds one frame to adjust its rate. That fits into 1023 bytes but not into 512.
3. An explicit feedback endpoint is an ISO IN endpoint. Using one forces the half split, so asynchronous mode is ruled out.
4. The I2S clocks come from a 3.072 MHz oscillator, the CS42L42 generates LRCLK, and the nRF52840 can only be the I2S slave. The oscillator cannot be trimmed, so the USB clock and the output clock inevitably drift apart.

**State of Zephyr (checked against v4.3.1 and main)**

- `usbd_uac2` only generates synchronous endpoints (clock source `sof-synchronized`) or asynchronous endpoints with feedback. It has no adaptive mode. Patch `zephyr-patches/0002` adds an `adaptive` property to the audio streaming interface: the synchronisation type is written as adaptive, no feedback endpoint is generated, and the packet size is computed for 49 frames (784 B).
- `udc_nrf.c` hard-codes ISOSPLIT to HalfIN and registers half the buffer as the ISO endpoint limit, so `ep_check_config` rejects 768/784 B endpoints. Patch `zephyr-patches/0001` adds `CONFIG_UDC_NRF_ISO_OUT_ONLY`: ISOSPLIT=OneDir, an ISO OUT limit of 1023 bytes, and no ISO IN endpoint registered.

`scripts/apply-zephyr-patches.sh` applies both patches to the Zephyr tree of the west workspace. It is safe to run repeatedly.

**Other constraints**

| Constraint | Consequence |
| --- | --- |
| Full-speed USB | 24 bit × 8 channels exceeds the packet limit, so the input is 16 bit. Mixing is done in float, the output is 24 bit |
| Cortex-M4F at 64 MHz, 256 KB RAM | The original plan estimated 15–20% CPU for the 8→2 mix plus a 2-channel ASRC. The measured figure is higher (about 38% for the audio thread, see "Audio CPU" in section 8) |
| No hardware reset | The bootloader has already started the watchdog (about 5 s, configuration locked); the firmware can only feed it. The Track 1 + Track 4 path back to the bootloader must always be kept |
| The bootloader occupies 0x00000–0x1FFFF, the page at 0xFF000 is reserved | The application is linked at 0x20000; NVS lives at 0xF7000–0xFEFFF (32 KB) |
| The bootloader leaves PWM2, PWM3 and the SAADC running | They are stopped and their pins disconnected at the very start of boot, before the Zephyr drivers take over |
| The TAS2505 is a mono amplifier and shares one I2S data line with the CS42L42 | The TAS2505's DAC data path selection produces (L+R)/2 (P0/R63 D5:D4 = 11) |

## 3. System architecture

Data path: USB ISO OUT → 8-channel int16 ring buffer (the only elastic buffer) → the audio thread pulls frames at the I2S rate → 8→2 mix (float) → ASRC → 24-bit I2S → CS42L42 / TAS2505.

Control path: the control thread (faders, buttons, LEDs, headphone detection, watchdog feeding) → lock-free parameter handover → audio thread. Rate estimation runs inside the audio thread; scene storage runs in a low-priority work queue; the CDC console runs in the lowest-priority thread.

The code has two layers:

- `src/core/`: plain C with no Zephyr dependency (mixer, ASRC, rate loop, button decoding, fader filtering, interaction state machine, scene serialisation, console command parsing). Unit tested on the host in `tests/host`.
- `src/platform/`: Zephyr glue (USB, I2S, I2C codecs, SAADC, PWM, watchdog, power, NVS, CDC).

## 4. USB design

Composite device: UAC2 + CDC ACM, grouped with interface association descriptors. Device class 0xEF/0x02/0x01. bMaxPower = 100 mA (the Lightning camera adapter rejects devices that declare more). VID and PID are configurable (`CONFIG_SP1M_USB_VID`, `CONFIG_SP1M_USB_PID`). The default is the pid.codes test ID 0x1209:0x0001; a registered PID is needed before builds are distributed.

**UAC2 topology**

1. Clock Source: internal-fixed, reports 48000 Hz only, read-only, not synchronised to SOF
2. Input Terminal: USB Streaming, 8 channels, spatial layout 7.1: FL FR FC LFE BL BR SL SR
3. Output Terminal: Headphones
4. No Feature Unit. Note: in Windows shared mode the volume slider becomes a software volume in the system mixer, so the user documentation asks for it to stay at 100%. How the iPad's volume control behaves has not been checked yet

| Channels | 7.1 position | Track |
| --- | --- | --- |
| 1–2 | FL / FR | Track 1 L/R |
| 3–4 | FC / LFE | Track 2 L/R |
| 5–6 | BL / BR | Track 3 L/R |
| 7–8 | SL / SR | Track 4 L/R |

Windows does not show channel names from the descriptors, so the channels are not named; this table is part of the user documentation instead. Track 2 lands on the LFE position, so M1 had to check that Windows shared mode does not low-pass it. In M1 the channel test tone on channel 4 arrived at its expected level (section 12).

**Audio streaming interface**

| Parameter | Value |
| --- | --- |
| Alt 0 | Zero bandwidth |
| Alt 1 | Type I PCM, bSubslotSize 2, bBitResolution 16, 8 channels |
| Endpoint | ISO OUT 0x08, adaptive, data |
| wMaxPacketSize | 784 bytes |
| bInterval | 1 |
| Feedback endpoint | None |
| ISOSPLIT | OneDir (patch) |

**CDC ACM**: a text line protocol. Commands (`help` lists them): `status`, `watch on|off`, `adc`, `threads`, `clear`, `scene save|load <1-4>`, `spk auto|off|on`, `audio coop|preempt` (full build), `vol <-60..0>|off`, `codec [cs <reg hex> | tas <page hex> <reg hex>]` (full build), `charge on|off`, `dfu`, `reboot`, `off`, `selftest stall|wdt`, `help`. The original plan's set was `status`, `scene save N`, `scene load N`, `charge on|off`, `spk auto|off`, `dfu`, `reboot` and `help`; the others were added for bring-up and diagnostics. The CDC console can be removed entirely with `CONFIG_SP1M_CDC=n`.

## 5. Real-time audio pipeline

There are two clock domains. On the USB side a packet arrives every 1 ms at the host's pace (47, 48 and 49 frames are all accepted). On the I2S side a block of 64 frames (1.33 ms) is needed at the oscillator's pace. Between them there is a single 8-channel int16 ring buffer: 1024 frames (about 21 ms), with a linearised fill target of 240 frames (5 ms), adjustable with `CONFIG_SP1M_TARGET_FILL`.

**Processing per I2S block (audio thread)**

1. From the ASRC's current phase and ratio, work out how many input frames N this block needs (about 64 ± 1)
2. Take N frames from the ring buffer; if there are not enough, pad with silence and count an underrun
3. Mix: per-track gain and balance coefficients are smoothed with a one-pole filter (about 5 ms) and interpolated per sample; mute and solo fade in and out over 5 ms; while any track is soloed, the others are muted
4. Level detection: per-track peaks for the LEDs
5. Master bus: master volume and the safety soft clipper
6. ASRC: polyphase Kaiser-windowed sinc (128 phases × 24 taps, linear interpolation between phases), ratio range ±1000 ppm. Error against an ideal resampler in host simulation: −102 dB at 1 kHz, −93 dB at 10 kHz, −89 dB at 16 kHz; coefficient table about 12 KB of RAM. The implementation now uses 20 taps with the coefficients stored together with their step to the next phase (a 20 KB table); see "Audio CPU" in section 8
7. Float to 24 bit, written to the I2S queue

Several of these steps were later restructured for speed (combined gains, per-block ramps, track meters taken from the USB thread); see "Audio CPU" in section 8.

**Rate estimation** (changed from the original plan: no GPIOTE counting on the LRCLK pin, which the I2S peripheral already uses)

- Feed-forward: TIMER2 runs freely at 16 MHz, and PPI captures timestamps on the USBD SOF event and on the I2S TXPTRUPD event. Over a long baseline (about 1 s) the length of a 1 ms USB frame is compared with the length of an I2S frame. This gives the ratio with a resolution better than 0.1 ppm.
- Feedback: a slow PI loop (time constant about 3 s, integral time 20 s) pulls the fill back to the target and absorbs the difference between the host's packet rate and SOF. Packets arrive every 1 ms and blocks are taken every 1.33 ms, so the raw fill is a staircase, and as the two clocks drift in phase its average swings by ±8 frames. The fill is therefore first linearised with the SOF timestamps and the frame number at which each packet was received (USB frames not yet delivered are counted as if they had arrived continuously). Host simulation: a host locked to SOF gives a ratio error of ±3 ppm, an unlocked host (+300 ppm) ±2 ppm, both with zero underruns.
- Start-up: when the stream first opens, the nominal ratio 1.0 is used, and output starts once the fill reaches the target.

**Speaker path**: TAS2505 P0/R63 = 0xB0 (DAC powered, input (L+R)/2). The plan was to confirm this on hardware in M2 and to fall back to 0x90 (left channel only) if it did not work; M2 confirmed (L+R)/2. The TAS2505 is muted while headphones are plugged in.

**Host stops playing** (Alt 0): the pipeline fades to silence, I2S keeps running, the ASRC and the estimator are reset, and the pipeline waits for the next Alt 1.

**Latency budget (device side, estimated)**: ring buffer about 3–5 ms (linearised target 5 ms; the actual fill moves between 3.5 and 5 ms as packets arrive) + ASRC group delay about 0.2 ms (0.25 ms with the original 24 taps) + an I2S queue of at most 4 × 64 frames, about 5 ms, for a total of about 9–10 ms. Once the underrun margin has been measured, `CONFIG_SP1M_TARGET_FILL` and `CONFIG_SP1M_I2S_BLOCKS` can be lowered. The real output latency has not been measured yet.

## 6. Control and interaction

The button ladder readings use the values measured by the sp1-tape-looper project (SAADC: gain 1/6, internal 0.6 V reference, 12 bit, 20 µs acquisition time, ladder powered from P1.10). Every combination of track keys on ladder 1 can be told apart (for example Track 1 + Track 4 reads about 1303). A decoded value is accepted only after two consecutive identical readings, and after a combination is released, single-key decoding resumes only once the ladder is really idle.

| Action | Function |
| --- | --- |
| Faders 1–4 | Track volume (logarithmic taper; the bottom ~2% is silence) |
| Tap a track key (acts on release, if the fader was not used for balance while the key was held) | Mute / unmute |
| FUNCTION + track key | Solo on / off (several tracks can be soloed) |
| Hold a track key and move that track's fader | Balance (relative mode); after release the fader controls volume again, with pickup |
| Volume + / − | Master volume in 1 dB steps; hold to repeat |
| Tap PLAY | Enter / leave the scene layer (it closes by itself after 3 s without input) |
| In the scene layer: tap track key N | Recall scene N |
| In the scene layer: FUNCTION + track key N | Save to scene N |
| Tap FWD or BACK | LED display mode: meter / status |
| FUNCTION + FWD or BACK | Speaker mode: auto / always off |
| Hold FUNCTION alone for 3 s (no other key during that time) | Power off |
| Hold Track 1 + Track 4 for 3 s | Back to the bootloader (in any mode; no version may remove this) |

A tap is a press released within 0.6 s with no other key involved apart from FUNCTION.

**Faders**: scanned at 500 Hz, median filter + low-pass + hysteresis, quantised to 0–1023, raw range 60–3600. Volumes are not updated while any ladder key is down and for 40 ms after it is released, because the keys pull down the supply that the faders share. After a scene recall or at power-up a fader is in pickup: it must pass the stored value before it takes over, and the track LED blinks slowly in the meantime. Pulling a fader down below the stored value takes over immediately (see "Output safety" in section 8).

**Balance law** (stereo tracks): both sides are at 0 dB in the centre. Moving towards the right attenuates the left side along a cosine down to zero, and the other way round.

**LEDs**

- Track LEDs: brightness follows the track's input level as sent by the host (before the fader); an audible track stays faintly lit; a muted track is dark; a soloed track blinks fast; a fader in pickup blinks slowly. While Track 1 + Track 4 are held, the four track LEDs ramp up to full over the 3 s. In the scene layer they show which slots hold a scene (dim) and the scene last saved or recalled (bright, with a short blink as confirmation).
- Playback LEDs: in meter mode an output level meter (steps at −30, −18, −9 and −3 dBFS); in status mode USB configured, streaming, headphones plugged in and charging (blinking). While the master volume changes they show it as a bar for 1.5 s. In the scene layer they pulse slowly. On an underrun they all flash briefly. While FUNCTION is held alone they count down to power-off.

The original plan had the playback LEDs show the scene number in the scene layer; the implementation shows the scene slots on the track LEDs instead.

## 7. Scenes and persistence

- Scene contents: volume, mute, solo and balance of the 4 tracks, master volume, speaker mode (16 bytes per scene). The last state, the four scene slots, the LED mode and the charging setting are stored together as one 89-byte record with a magic number, a version number and a CRC-16; a record that fails these checks is ignored and the defaults are used.
- Storage: NVS at 0xF7000–0xFEFFF (8 pages) by default. That is also where other SP-1 firmware keeps its own NVS log (feldd's profiles), so the build can place it in its own 16 KB at 0xCA000–0xCDFFF instead (`SP1M_STORAGE=low`, `boards/teenage_engineering/sp1/sp1-storage-low.overlay`; the image then has 0x20000–0xC9FFF). The settings record has its own NVS id, so the two can share the default region, but they share one log (free space, garbage collection) and `store_init()` starts over when a mount fails. Flashing new firmware through the bootloader does not erase this area: on 2026-10-19, feldd's NVS data at 0xF7000–0xFEFFF was byte-identical after flashing SP-Mix (low layout) and then feldd again (PR #2). An update from one SP-Mix version to another has not been checked directly.
- **No flash writes during playback**: a scene save is first kept in RAM (the LEDs confirm it), and the "last state" is marked dirty once the parameters have been unchanged for 5 s. The actual write happens only when the stream stops (Alt 0), when USB is not configured, in standby, or before power-off. Zephyr's partial erase is enabled as an extra safeguard.

## 8. Threads, power, safety and recovery

| Thread / context | Priority | Role |
| --- | --- | --- |
| USBD interrupt + `udc_nrfx` / `usbd` threads | Zephyr default (cooperative, `K_PRIO_COOP(8)`) | Receive ISO packets; the callback only copies into the ring buffer and takes per-channel peaks |
| Audio thread | Cooperative `K_PRIO_COOP(7)` (−9): above the USB threads and the system work queue. `audio preempt` switches it to preemptible 0 for comparison | Per block: rate estimation, mix, ASRC, write to I2S. A block (1333 µs) takes about 500 µs in practice, about 600–620 µs when everything is clipping at full scale |
| Control thread (main) | Preemptible 4, 500 Hz | Faders and buttons, interaction state machine, LEDs, headphone detection, watchdog feeding |
| Storage work queue | Preemptible 8 | NVS reads and writes |
| Console thread | Preemptible 12 | Command parsing, status output |

The original plan put the audio thread at preemptible 0, below the cooperative USB threads and above everything else. On the first full-build hardware run, bursts of USB and console work then delayed blocks long enough to cause I2S underruns, so the audio thread now runs cooperatively above them (section 12). A block (about 0.5 ms) delays the USB threads instead; `udc_nrfx` has about one USB frame to arm the next ISO OUT buffer after each packet, so one block fits.

**Watchdog**: started by the bootloader, about 5 s, configuration locked. If the firmware finds it not running at boot (for example when started from a debugger), it starts it itself with 8 s. The control thread checks every 100 ms and feeds the watchdog only while both the audio thread and the control loop are making progress. The audio thread waits for I2S with a timeout and never blocks forever.

**Fault records**: the fatal error handler writes the reason and the PC into `__noinit` RAM. The reset culprit code is written both to GPREGRET2 and to `__noinit` RAM and reported by `status` at the next boot (`kept` says where it was read back from). Measured with `selftest`: after a soft reset through the bootloader both copies survive; a watchdog reset clears GPREGRET2 but keeps RAM, so a watchdog culprit can only be read back from RAM (this is why the early M0/M1 watchdog resets all showed "none"). The address of the record depends on the build (in the full build it is near 0x20010190, which differs from the m1 build), so when moving to a new build, run `selftest stall` once to confirm that the records still survive.

### Stall catcher

The control loop marks the stage it is in and counts its iterations. The USB SOF callback (usbd thread, every 1 ms) and a 50 ms kernel timer each watch the count. If it does not move for 1.5 s, the watcher records the stage, the object the main thread is waiting on, the code addresses found on its stack, and the SAADC, RTC1, NVIC and PPI registers; writes GPREGRET2 = 0x60 | stage (0x70 | stage if the kernel timer had stopped as well); and performs a soft reset itself, before the watchdog fires, so that the record survives. `status` prints the record as three `stall` lines, and the web console keeps them with its reboot history.

### SAADC failure handling

A failed scan is only counted and retried. After 3 failures in a row the SAADC is power-cycled (the same approach nrfx uses for anomaly 212). If there has been no good scan for 40 ms, the ladder keys are treated as released (the UI is told, so this does not count as a tap); FUNCTION has its own pin and always works. If there is still no good scan after 1 s, the state is saved and the firmware resets once (culprit 0x49). If the previous reset had the same cause and no scan has succeeded since this boot, it does not reset again.

### CPU accounting

The thread statistics count DWT cycles. The DWT cycle counter stops while the CPU sleeps in WFI, so raw percentages are only shares of the time the CPU was awake. `threads` (and the `threads` line of `status`) now scale by the elapsed wall-clock time and report `sleep` separately; the web console's "CPU busy" figure is 100 − idle − sleep. Measured in M1: the control loop takes about 55–65 µs per iteration (`main us`), about 3% of the CPU.

### Audio CPU

Before the first hardware run, a simulator estimated the Cortex-M4 cycle counts: the original implementation needed about 1146 µs per block (64 frames, 1333 µs), which together with the USB thread would overload the CPU. The following changes brought it down:

- The toolchain's memcpy/memset are byte-by-byte loops. They were replaced with word copies, built with `-fno-tree-loop-distribute-patterns` so that GCC does not turn the loops back into library calls.
- The float-to-I2S conversion (to_i2s) uses a single VCVT truncation instead of a call to lrintf per sample.
- The mixer combines track gain × master volume × 1/32768 into 8 gains held in registers, ramped linearly within a block, and the DC blocker runs in the same loop.
- The track meters use the per-channel peaks that the usbd thread already computes when it receives a packet, so the mixer no longer scans the input.
- The ASRC coefficient table stores each coefficient interleaved with its difference to the next phase, the inner loop handles 4 taps per iteration, and the filter was reduced to 20 taps with a retuned Kaiser window (β 9.1). Error: −97 dB at 1 kHz, −109 dB at 5 kHz, −95 dB at 10 kHz, −91 dB at 16 kHz, all within the original thresholds.

A review then added to_i2s back into the estimate from its measured instruction count (21 cycles per sample). In total: about 503 µs (38%) per block, practically the same for every block with no spikes; about 600–620 µs (47%) when every frame goes through the soft clipper. The usbd thread went from 85 µs to 23 µs per packet.

### Output safety

- At power-up and when headphones are plugged in, the master volume is capped at −20 dB. Recalling a scene never raises the master volume. The default is −24 dB.
- The CS42L42 mixer adds a fixed 12 dB of attenuation (`CONFIG_SP1M_HP_ATTEN_DB`).
- Pulling a fader down below its stored value takes over immediately.
- Soft clipping comes after the mix and the 5 Hz DC blocker. In the other order the DC blocker would push clipped low frequencies past full scale, and twice as many samples would hit the hard clip in to_i2s. The ASRC still overshoots a little on clipped signals, so the clamp in to_i2s still acts occasionally. With heavy asymmetric clipping the speaker gets a little DC; this happens with either order, and only under obvious overload with audible distortion.
- The CS42L42's DAC high-pass filter is enabled (register 0x1F06, only bit 1 changed).
- Headphone detection is read every 50 ms. The first reading that says "plugged in" lowers the master volume (three failed reads in a row also count as possibly plugged in and lower it once), while the speaker switches only after three consistent readings. At power-up a majority vote decides (a tie counts as plugged in); a failed read keeps the previous state.
- If the CS42L42 did not initialise completely (jack detection not configured), the jack is not read and the headphone amplifier is not enabled; only the speaker is used then. The speaker stays off until the jack state is known, and a failed speaker switch is retried.
- I2C "page select + read/write" sequences are locked, so a console register read cannot slip into the middle of an access by the control thread.
- Planned resets (`reboot`, `selftest`) are prepared by the control thread: mute, wait 30 ms, reset both chips, switch off the oscillator (the order that the sp1-tape-looper project verified on hardware to be pop-free). The stall catcher, crashes and the watchdog interrupt have no time to mute: they pull both chips into reset and switch off the oscillator directly, and hold that for 50 µs before resetting the CPU, so that the reset pulse is long enough.
- The "pull down to take over" check for faders only looks at whether the raw ladder reading is idle (it counts as pressed after 2 consecutive non-idle readings, so single noisy readings do not matter) instead of waiting for the debounce. This keeps the voltage drop caused by a key press from being taken for a fader being pulled down.

### Driving the SAADC directly (root cause of the M1 stalls)

In M1 the control loop stalled every few seconds inside Zephyr's `adc_read()`. The stall records showed the main thread waiting forever on the driver's completion semaphore, with the SAADC enabled but no events and no result. The cause is in nrfx: `nrfx_saadc_mode_trigger()` triggers TASKS_START first, and the compiler placed the store `saadc_state = SIMPLE_MODE_SAMPLE` after it (`nrf_barrier_w()` is empty on Arm). An interrupt between those instructions (in M1, the USB SOF every millisecond) lets the STARTED interrupt see the old state: it clears the event without triggering SAMPLE, and END never comes. `inputs.c` now drives the SAADC registers directly, polled, without interrupts and without waiting: each call collects the result of the previous scan and starts the next one; if a start or the end of a scan times out, the SAADC is reset and the event is counted (the `adc` fields of `status`).

### Recovery paths

1. Hold Track 1 + Track 4 for three seconds while the firmware runs: it writes GPREGRET = 0x57 and performs a soft reset. The user keeps holding the keys, and the bootloader detects them itself and enters update mode. The check runs first in the control thread and does not depend on the mode.
2. Hold Track 1 + Track 4 while plugging in USB: built into the bootloader, always works.
3. Console command `dfu`: writes GPREGRET = 0x57, then performs a soft reset. Verified in M0: the bootloader honours this value and enters update mode directly.

The M0 gate only requires paths 1 and 2. All three have been verified on hardware.

### Power

- Boot policy: the bootloader starts the application after any power-up event. With USB connected, the firmware goes straight into the mixer; when woken by FUNCTION, by the watchdog or by a soft reset, it also goes into the mixer; on battery only after a power-on reset, it goes straight to SYSTEM_OFF.
- Charging: nCE is pulled low at boot to enable charging (otherwise the battery never charges). The BQ24232 input current limit is fixed at 500 mA in hardware; the firmware can only switch charging on or off (`charge on|off`).
- Power-off: fade out → mute and reset both amplifiers → switch off the 3.072 MHz oscillator → switch off the eMMC VCCQ supply → keep the Bluetooth module in reset → LEDs off, ladder supply off → wait for FUNCTION to be released → set FUNCTION as the wake-up source → clear RESETREAS → SYSTEM_OFF. With USB connected, SYSTEM_OFF would have no wake-up edge, so the firmware enters an in-application standby instead (showing charging only); holding FUNCTION for 0.6 s powers it back on, and once USB is unplugged it goes to SYSTEM_OFF.
- No host connected (USB not configured) and no input for 10 minutes: power off.
- The Bluetooth module is always held in reset.

## 9. Milestones and verification

The order follows risk: first make sure the device can always be reflashed, then immediately verify the USB part, which is the most likely to break the design and needs neither the codecs nor the ASRC, and only then the interaction details. Each stage has a build variant (`scripts/build.sh <variant>`).

| Stage | Content | Build variant | Gate (must pass on hardware) |
| --- | --- | --- | --- |
| M0 Safety base | Board definition, link address 0x20000, watchdog, fault records, LED heartbeat, button ladders, recovery paths, power-off and standby, CDC | `m0` | Recovery paths 1 and 2 work; normal quiescent current after power-off; a conclusion on whether `dfu` works |
| M1 USB probe | Patched UDC + adaptive 8-channel UAC2; data is only counted, not played | `m1` | iPad and Windows enumerate 8 channels; packet size and loss counters normal over 30 minutes; a conclusion on how Windows treats LFE |
| M2 Audio output | CS42L42 / TAS2505, 24-bit I2S, rate estimation, ASRC, playing Track 1 | `full` | 30 minutes of playback on both platforms with zero underruns; speaker (L+R)/2 confirmed |
| M3 Mixing and interaction | 8-channel mix, mute, solo, balance, fader pickup, LED meters, all mappings | `full` | Every entry of the mapping table passes; no zipper noise on fast fader moves; per-channel routing correct in AUM and Reaper |
| M4 Scenes and CDC | NVS scenes, last state, writes only while stopped, CDC commands | `full` | Scenes survive power-off after playback has stopped; no pops when saving during playback |
| M5 Stability and release | 8 hours of continuous playback, repeated plugging and power cycling | `full` | 8 hours with zero underruns and zero resets; recovery paths re-tested before release |

**Verification methods**: the CDC `status` command (every second with `watch on`) reports underruns, overruns, packet loss, the ASRC ratio (ppm), the ring buffer fill, the longest block time and the reset reason; an external audio interface records the headphone output for sweeps and 1 kHz tests; per-channel test tones confirm the routing; two audio interfaces compare latency. A known-good binary is kept for every stage. The step-by-step procedure is in [TESTING.md](TESTING.md).

## 10. Risks and open questions

| Risk | Mitigation | Status |
| --- | --- | --- |
| 784-byte packets still truncated or lost after the UDC patch | M1 checks the packet size histogram in `status` | Windows sends 48-frame (768 B) packets, received without gaps; 49-frame packets have not been seen |
| The bootloader does not honour GPREGRET = 0x57 | Recovery path 3 degrades to a plain reboot; paths 1 and 2 are unaffected | Honoured: `dfu` enters update mode |
| The bootloader erases the NVS area when flashing | Scenes return to their defaults; documented for users | Unknown |
| TAS2505 (L+R)/2 does not work | Fall back to the left channel | Works |
| Windows processes the LFE channel (Track 2 right) | Use a different bmChannelConfig layout; decided in M1 | The 1 kHz test tone on channel 4 arrived at its expected level in M1 |
| No ASIO on Windows | FlexASIO, or a DAW with native multichannel WASAPI (Reaper) | Workaround documented |
| iPad apps that only output stereo | Use an app with multichannel routing, such as AUM | Workaround documented |
| Not enough power from the iPad | bMaxPower 100 mA; `charge off` disables charging | Workaround documented |

## 11. Changes from the original plan

1. Zephyr does not support adaptive endpoints, and the UDC driver hard-codes the half ISOSPLIT: both are solved by patches kept in this repository, which turns them from "to be evaluated" into defined tasks
2. Rate estimation uses TIMER captures of SOF and of the I2S TXPTRUPD event instead of GPIOTE on the LRCLK pin, which the I2S peripheral already uses
3. Only one elastic buffer (the 8-channel ring buffer); the ASRC pulls on demand
4. Watchdog: started by the bootloader, about 5 s, configuration locked; fault records also use GPREGRET2
5. The CDC `dfu` command is best effort; the M0 gate only requires the button recovery paths
6. The charging current cannot be selected, only switched on and off; nCE must be pulled low; bMaxPower is 100 mA
7. No flash writes during playback; writes are deferred to stop, standby or power-off; partial erase is only an extra safeguard
8. The power-off sequence powers down all peripherals; boot policy and standby with USB connected added
9. Channel layout fixed as 7.1; channel naming dropped; the statement that the host volume slider has no effect was corrected
10. Balance law changed to 0 dB in the centre; power-off changed to holding FUNCTION alone for 3 s; mute acts on release; balance uses a relative mode
11. All button ladder combinations can be told apart (measured); the measured thresholds are used
12. Milestones reordered: the USB probe moved up to M1 and tests 8 channels directly; every stage has a build variant

## 12. Implementation status (2026-10-15)

The code implements this design. The three build variants (`m0`, `m1`, `full`) build without warnings, and the portable core has host unit tests (`tests/host`, over 800,000 checks).

### Verified on hardware

**M0 safety base**

- Recovery paths: holding Track 1 + Track 4 for 3 s while the firmware runs, holding them while plugging in USB, and the `dfu` console command all reach the bootloader. The bootloader honours GPREGRET = 0x57, so `dfu` enters update mode without any key.
- Reset records: a soft reset keeps the culprit code in both GPREGRET2 and RAM; a watchdog reset clears GPREGRET2 but the RAM copy survives (`selftest wdt`). In the full build the stall record survives as well (`selftest stall`: register and memory copies both kept). Watchdog, stall and crash records survive resets.
- Fader range and ladder thresholds were checked with the web console on one SP-1: every key falls within its expected band (PLAY is 20 counts from the upper edge of its band), and the faders read from 0 at the bottom to 3700–3820 at the top. Keys pull the shared supply down by 2–4% and the fader readings drop with it, which is why faders do not set volumes while a key is down, and why the balance gesture is only evaluated 50 ms after a track key is pressed.
- The control loop stalls every few seconds in M1 (followed by watchdog resets) were traced to a race in the Zephyr/nrfx SAADC driver (section 8) and fixed by driving the SAADC directly. Since then the firmware runs without resets and the ADC counters are clean.
- An apparent control loop CPU load of about 36% at idle was an accounting artefact (section 8, "CPU accounting"); the real figure is about 3%.

**M1 USB**

- Windows and the iPad both enumerate the SP-1 as an 8-channel device.
- On Windows the channel test tones (−3 … −24 dBFS) arrive at their expected levels on every channel, including channel 4 at the LFE position. All packets are 48 frames, `gaps`, `part` and `nobuf` stay at 0, the ADC counters are clean, and the CPU is about 26% busy while a stream plays (m1 build: data counted, not played).

**M2 audio output (full build)**

- The CS42L42 and the TAS2505 come up on the first attempt (0x1109 = 0x02, 0 I2C errors). The speaker plays (L+R)/2. Headphone detection switches the speaker off and caps the master volume at −20 dB on insertion.
- Faders, mute, solo, balance, scenes and the LED meters have been tested on hardware.
- First full-build run: 1–2 I2S dropouts about every 9 minutes, with up to 3.3 ms between blocks. The audio thread (then preemptible 0) ranked below the cooperative usbd, udc_nrfx and system work queue (console output) threads and was held off by bursts of their work. The audio thread now runs cooperatively above them, with 4 I2S blocks in flight.
- Second run (Windows, about 12 minutes including one stop and restart): 0 I2S errors, 0 underruns; playing blocks took 495 µs on average and 761 µs at most, of the 1333 µs block period; the longest time between two blocks was 1.63 ms; audio thread 38%, usbd 11%, total CPU busy 63%; USB 48 µs per packet.
- The `gaps 6144` seen in the second run came from reading FRAMECNTR just as it updated, which gave a bad frame number (packets plus gaps exceeded the number of elapsed frames). The counter is now read until two reads agree, and steps of more than 16 frames are counted separately as `jumps`.
- The full build also works on the iPad.
- The first `status` line now includes the firmware's git hash (`fw <hash>`), so a test record shows which build it came from.

### Known host behaviour

- Windows: if the SP-1 re-enumerates (reflash, reboot, `selftest`, replug) while a player is open, the player's existing stream is moved back as stereo and only Track 1 (channels 1–2) plays. Stop and start playback again, select the SP-1 explicitly as the output device, or use WASAPI exclusive mode or ASIO through FlexASIO. iPad apps reconfigure automatically.
- Windows shared-mode volume should stay at 100% (section 4).

### Open

- The 30-minute runs of the M1 and M2 gates
- The 8-hour soak of M5, and repeated plugging and power cycling
- Scene retention across power-off (NVS) is untested (M4 gate); a saved scene survives a reboot
- Keeping the settings across an update from one SP-Mix version to another has not been checked directly (reflashing leaves the 0xF7000 region intact, section 7)
- The real output latency has not been measured
- No results have been recorded yet for the quiescent current after power-off (M0 gate), for zipper noise on fast fader moves and per-channel routing in AUM and Reaper (M3 gate), or for clicks when saving during playback (M4 gate)
- How the iPad's volume control acts on the device has not been checked (section 4)

### Implementation note

Full build: `nrfx_i2s_next_buffers_set` writes TXD.PTR before it updates its state, so an interrupt in between is taken for an underrun (counted in `i2s`) and is recovered by the restart in `audio.c`. Do not wrap it in `irq_lock`: that would turn an underrun that is detected into one that silently plays wrong data.

## References

- [SP-1-dev](https://github.com/timknapen/SP-1-dev) by Tim Knapen (MIT): pins, I2S, I2C initialisation, bootloader constraints
- [sp1-tape-looper](https://github.com/chattock/sp1-tape-looper) by chattock (MIT): Zephyr + UAC2 implementation, measured button ladder values, power-off and recovery approach
- [feldd](https://github.com/bnjreece/feldd-sp1-firmware) by Benjamin Reece (MIT): watchdog, boot policy, TRS jack pins
- Their license notices: [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)
- [Zephyr v4.3.1 udc_nrf.c](https://github.com/zephyrproject-rtos/zephyr/blob/v4.3.1/drivers/usb/udc/udc_nrf.c), [usbd_uac2_macros.h](https://github.com/zephyrproject-rtos/zephyr/blob/v4.3.1/subsys/usb/device_next/class/usbd_uac2_macros.h)
- [Microsoft: USB Audio 2.0 drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers)
- [TI TAS2505 application reference guide (SLAU472C)](https://www.ti.com/lit/ug/slau472c/slau472c.pdf)

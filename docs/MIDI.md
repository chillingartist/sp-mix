# MIDI control

An optional USB-MIDI port lets a DAW, an app or a hardware controller drive
the mixer: track volume, balance, mute and solo, the master volume and scene
recall. As a mixer it is **receive only**: it sends no MIDI. In
[controller mode](#controller-mode) the roles turn around: the faders and
buttons send to the DAW, and the DAW mixes. MIDI is off by default
(`CONFIG_SP1M_MIDI`); without it the firmware is unchanged. The design is
human-led, with AI assistance on the implementation.

```sh
SP1M_MIDI=1 scripts/build.sh full                        # channel 1
SP1M_MIDI=1 SP1M_MIDI_CHANNEL=5 scripts/build.sh full
```

Releases from v0.1.10 on also have this build ready made, on channel 1:
`sp-mix-full-midi.bin`.

The SP-1 then shows up as a MIDI device named "SP-Mix" beside the audio
device and the serial console.

| Function | Message on the selected channel |
| --- | --- |
| Track 1–4 volume | CC 20–23, LSB on CC 52–55 |
| Track 1–4 balance | CC 24–27, LSB on CC 56–59; 64 (8192 in 14 bits) is centre |
| Master volume | CC 28, LSB on CC 60 |
| Track 1–4 mute | CC 102–105; 64–127 on, 0–63 off |
| Track 1–4 solo | CC 106–109; 64–127 on, 0–63 off |
| Scene recall | Program Change 0–3 means scene 1–4 |

Other messages and channels are ignored. Mute and solo are absolute states,
so repeating a message does not toggle them. Every Program Change 0–3 recalls,
repeats included; 4–127 do nothing. Scene save over MIDI (CC 110–113 in the
proposal) is not implemented.

**Resolution.** A 14-bit value is the MSB on CC n and the LSB on CC n+32, in
either order. The MSB alone is a plain 7-bit value and reaches full scale, so
a sender that never sends an LSB works too. Once an LSB has arrived, later
MSBs keep it.

## How it fits the mixer

A MIDI message changes the **same state** the buttons and faders change, so
everything that applies to them applies to it: the state is saved to flash
and stored in scenes, the LEDs show it, and the volume limits hold (the
master is capped at −20 dB at power-up and when headphones go in, and a
scene recall never raises it).

- **Volume** maps 0–16383 onto the fader's 0–1023 and the fader's own curve.
  After a MIDI volume change the physical fader is ignored until it reaches
  the new value, with the slow track LED blink, exactly as after a scene
  recall (pulling a fader down still takes over at once).
- **Balance** maps onto the mixer's −100..+100. A lone 64 and a 14-bit 8192
  are both centre, and both resolutions reach both ends.
- **Master** maps 0–16383 evenly onto off, −60..0 dB in the mixer's 1 dB
  steps.
- The last writer wins: a button press or fader move after a MIDI message
  changes the state as usual, and the next MIDI message changes it again.

**Where it runs.** The USB-MIDI class receives in the USB thread and only
decodes and queues (64 messages; a full queue drops and counts). The control
thread applies the queue once per 2 ms tick, before the UI tick, so MIDI and
the buttons and faders are applied by one thread. Nothing runs in the audio
thread, and the MIDI endpoints are bulk, so they do not touch the isochronous
audio buffer. `status` prints a `midi` line: packets received, messages
queued, applied and dropped, and for controller mode the messages ignored
(echoes, program changes), sent, and lost before sending.

## Controller mode

In controller mode the SP-1 is a control surface for the DAW, and the DAW
does the mixing. The faders and buttons send the same messages the mixer
receives (the table above), so a DAW maps them with MIDI Learn, and what it
records can be played back to the SP-1. The SP-1 passes the host's audio at
0 dB on all four tracks and applies only its master volume, so nothing is
attenuated twice. Send the DAW's mix to outputs 1–2.

| Control | In controller mode |
| --- | --- |
| **FUNCTION + tap PLAY** | Controller mode on / off (also `mode ctrl` / `mode mixer` in the console). The playback LEDs run left to right when it comes on and right to left when it goes off. |
| Fader 1–4 | CC 20–23 (7-bit) |
| Tap a track button | Mute: CC 102–105, a button press (127, then 0) |
| Hold PLAY + tap a track button | Solo: CC 106–109, a button press (127, then 0). FUNCTION + tap works too, as in the mixer. |
| Hold PLAY + move fader N | Balance of track N: CC 24–27, absolute (bottom left, middle centre, top right). The fader takes over once it has passed the current balance; until then its track LED blinks, then stays lit. The volume is not touched. |
| VOL+ / VOL− | The SP-1's master volume, as always. Not sent. |
| Tap PLAY (no fader moved) | Send the four fader volumes again (also `midi dump`). The playback LEDs flash. |
| Scenes | Not used. Program changes from the host are ignored. |

- **The mode is not saved.** The SP-1 always starts as the mixer. The mixer's
  own state (volumes, mute, solo, balance, scenes) stays as it was and comes
  back when the mode is left; the faders then pick up the mixer's volumes.
  The master volume, the speaker mode and the LED mode carry over both ways.
- **The DAW lines up at once.** Entering the mode, tapping PLAY, and USB
  coming up while in the mode (after 300 ms) send the four fader volumes.
  Balance, mute and solo have no position on the SP-1, so they are sent only
  when they change and the DAW keeps its own settings until then.
- **The PLAY layer.** While PLAY is held the faders are balance controls and
  the track buttons solo. A fader takes over the balance only after passing
  it, so the balance never jumps and can be set wherever the volume is; the
  middle of the travel (64 ± 2) is exact centre. The volumes stay as they
  were, and after PLAY is let go the faders pick them up again (slow LED
  blink). Holding a track button and moving its fader still changes the
  balance relatively, as in the mixer.
- **Buttons toggle in the DAW.** Every tap of a track button sends 127 and
  then 0, as a controller's button does, so a DAW that toggles on 127 (Logic's
  Learn MIDI, for one) mutes or solos on one tap and unmutes on the next. The
  track LEDs follow the SP-1's own copy of the state, or the DAW's state when
  the DAW sends it back.
- **No flooding.** A fader's value changes only once it is 2 fader units past
  a 7-bit step, and each fader and balance sends at most every 10 ms (the
  latest value).
- **No loops.** A value that came in over MIDI is never sent back. What comes
  in for a control within 300 ms of it moving or sending is taken as its echo
  and ignored, so a DAW that sends the messages back (MIDI thru, input
  monitoring) does not pull a fader that is being moved.
- **The DAW's automation.** A volume that comes in from the DAW moves the
  DAW's side only. The physical fader then waits for it, with the slow LED
  blink, exactly as after a scene recall, so the DAW's fader does not jump
  when you take over. Mute and solo from the DAW show on the track LEDs.
- **The LEDs** show the DAW's side: the track LEDs show mute, solo, pickup and
  the incoming level, and in meter mode the first playback LED stays dimly lit
  as a reminder that the SP-1 is in controller mode.
- The console's `ctrl` line shows the DAW's side and ends with `mode ctrl`.

## Vendored code

`src/core/midi/` holds `midi_msg.h`, `midi_hr.[ch]`, `midi_bus.[ch]`,
`midi_assign.[ch]`, `midi_params.[ch]` and `usb_rt_parse.[ch]` (portable C,
host-tested), and `src/platform/usb_midi1.[ch]` is the USB-MIDI 1.0 class for
Zephyr's device stack. Their tests are in `tests/host/midi/`. These are copied
byte for byte from the MIDI code of a feldd-based SP-1 firmware, where the same
class has run on macOS, Windows and iOS hosts; `VENDORED.md` records the
source commit and the SHA-256 of each file. The mixer's own code is
`src/core/midi_map.[ch]` (the message map) and `src/platform/midi_in.[ch]`
(the queue between the USB thread and the control thread, and the send path),
and `src/core/ctrl.[ch]` (controller mode).

The code is MIT licensed. The USB-MIDI class, the packet decoder
(`usb_rt_parse`) and the `midi_msg` struct are Benjamin Reece's, from feldd;
the receive hook in the class, the extraction and the remaining MIDI modules
and tests are by Ryan Gilmore. The original file notices travel with the
copies; see also the repository's `THIRD_PARTY_NOTICES.md` and `LICENSE`.

The copies are refreshed by an export script kept with that firmware. It checks all
first-line copyright notices before copying anything and overwrites only its
listed files and manifest. Re-running it with the same inputs is idempotent.

## Host tests

From the repository root:

```sh
cmake -S tests/host -B /tmp/sp1m-host
cmake --build /tmp/sp1m-host
ctest --test-dir /tmp/sp1m-host --output-on-failure
```

The existing `sp1m_tests` runner includes the mixer map tests. The five
vendored tests each have their own executable and CTest entry.
They and the vendor modules use AddressSanitizer and UndefinedBehaviorSanitizer
when the compiler and linker support them. Their assertions remain enabled in
Release builds. The existing mixer runner retains its sanitizer requirements.

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write an 8-channel 7.1 test WAV for checking the SP-1 Mixer controls.

Each track (channels 2N-1 and 2N) plays its own note in its own rhythm,
the same on left and right, so a faded, muted, soloed or panned track is
easy to hear and the track meters move:

    Track 1  C4, once a second        Track 3  G4, long notes
    Track 2  E4, twice a second       Track 4  C5, fast ticks

Each track peaks at -14 dBFS: all four together stay below the soft clipper
at master 0 dB. The rhythms repeat every 3 s, so the file loops cleanly.

    python3 mixer-test.py [out.wav] [seconds] [sample rate]
"""

import math
import struct
import sys

DEFAULT_RATE = 48000
CHANNELS = 8
# FL FR FC LFE BL BR SL SR, the order the device declares
CHANNEL_MASK = 0x63F
KSDATAFORMAT_SUBTYPE_PCM = bytes.fromhex("0100000000001000800000aa00389b71")

# (Hz, period s, note length s)
TRACKS = [
    (261.63, 1.00, 0.40),
    (329.63, 0.50, 0.20),
    (392.00, 1.50, 0.90),
    (523.25, 0.25, 0.08),
]
LEVEL = 10 ** (-14 / 20)
FADE_S = 0.005


def envelope(t, period, length):
    ph = math.fmod(t, period)
    x = max(0.0, min(1.0, ph / FADE_S, (length - ph) / FADE_S))

    return 0.5 - 0.5 * math.cos(math.pi * x)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "sp1-mixer-test.wav"
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
    rate = int(sys.argv[3]) if len(sys.argv) > 3 else DEFAULT_RATE
    seconds = max(3.0, round(seconds / 3.0) * 3.0)  # whole loops
    frames = int(rate * seconds)

    data = bytearray()
    for i in range(frames):
        t = i / rate
        s = []
        for f, period, length in TRACKS:
            v = int(round(LEVEL * envelope(t, period, length) *
                          math.sin(2 * math.pi * f * t) * 32767))
            s += (v, v)
        data += struct.pack("<8h", *s)

    fmt = struct.pack("<HHIIHHHHI16s", 0xFFFE, CHANNELS, rate,
                      rate * CHANNELS * 2, CHANNELS * 2, 16, 22, 16,
                      CHANNEL_MASK, KSDATAFORMAT_SUBTYPE_PCM)
    with open(out, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(data)))
        f.write(b"WAVE")
        f.write(b"fmt " + struct.pack("<I", len(fmt)) + fmt)
        f.write(b"data" + struct.pack("<I", len(data)) + data)
    print(f"wrote {out}: {seconds:g} s at {rate} Hz, 8 channels, "
          f"mask 0x{CHANNEL_MASK:X}")


if __name__ == "__main__":
    main()

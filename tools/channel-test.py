#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write an 8-channel 7.1 test WAV for checking the SP-Mix routing.

Every channel plays a 1 kHz tone at the same time, channel N at -3*N dBFS
(channel 1 -3 dB ... channel 8 -24 dB). With the host volume at 100 %, the
console's per-channel peaks should read -3, -6, ..., -24 in order. Channel 4
is the LFE position: if Windows low-passed it, it would read far below -12.

    python3 channel-test.py [out.wav] [seconds] [sample rate]

A low sample rate (e.g. 8000) keeps long files small; the host resamples to
the device's 48 kHz and the 1 kHz peaks stay the same.
"""

import math
import struct
import sys

DEFAULT_RATE = 48000
CHANNELS = 8
# FL FR FC LFE BL BR SL SR, the order the device declares
CHANNEL_MASK = 0x63F
KSDATAFORMAT_SUBTYPE_PCM = bytes.fromhex("0100000000001000800000aa00389b71")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "sp1-channel-test.wav"
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
    rate = int(sys.argv[3]) if len(sys.argv) > 3 else DEFAULT_RATE
    frames = int(rate * seconds)
    gains = [10 ** (-3 * (ch + 1) / 20) for ch in range(CHANNELS)]
    fade = rate // 50  # 20 ms in and out, no clicks
    # Whole cycles of 1 kHz per period, computed once and repeated
    period = [math.sin(2 * math.pi * 1000 * i / rate) for i in range(rate)]

    data = bytearray()
    for i in range(frames):
        env = min(1.0, i / fade, (frames - 1 - i) / fade)
        s = period[i % rate] * env
        data += struct.pack("<8h", *(int(round(s * g * 32767)) for g in gains))

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

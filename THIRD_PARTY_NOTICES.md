# Third-party notices

SP-Mix is written from scratch, but it builds on hardware knowledge, measured values and register sequences published by the projects below. Where this project uses material from them (pin assignments, initialization sequences, measured thresholds, power-handling approaches), their copyright and permission notices are reproduced here as their licenses require. Thanks to their authors.

## SP-1-dev, by Tim Knapen

<https://github.com/timknapen/SP-1-dev> and its wiki.

Used for: the SP-1 pin assignments (`boards/teenage_engineering/sp1/sp1-pinctrl.dtsi`, from `src/stemplayer_pins.h`), the I2S and I2C wiring and bootloader constraints (`boards/teenage_engineering/sp1/sp1.dts`), and the CS42L42 initialization sequence and headphone jack detection register (`src/platform/codec.c`).

```
MIT License

Copyright (c) 2026 Tim Knapen

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## sp1-tape-looper, by chattock

<https://github.com/chattock/sp1-tape-looper>

Used for: the measured button ladder levels and SAADC settings (`src/core/buttons.c`, `src/core/buttons.h`), the TAS2505 setup with its PLL locked to the I2S bit clock (`src/platform/codec.c`), the pop-free power-down order, and the approach to jack detection, power-off and recovery.

```
MIT License

Copyright (c) 2026 chattock

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## feldd-sp1-firmware, by Benjamin Reece

<https://github.com/bnjreece/feldd-sp1-firmware>

Used for: the handling of the watchdog started by the bootloader, the power-up policy, and the TRS jack pins. With MIDI control (`CONFIG_SP1M_MIDI`), also copied, with changes: the USB-MIDI 1.0 class (`src/platform/usb_midi1.[ch]`), the USB-MIDI packet decoder (`src/core/midi/usb_rt_parse.[ch]` and its test) and the `midi_msg` struct (`src/core/midi/midi_msg.h`). Only the original feldd work by Benjamin Reece, which is MIT licensed, was consulted; none of the third-party components bundled with feldd are used here.

```
MIT License, reproduced below, which covers ONLY
the original feldd code authored by Benjamin Reece.

The flashable firmware image (feldd.bin) and this repository also incorporate
third-party components that are NOT covered by this MIT License and remain
subject to their own licenses and their owners' rights. These include, without
limitation, the Infineon / Cypress CYW20706 radio firmware image and download
minidriver (Infineon proprietary), Zephyr RTOS (Apache-2.0), the Nordic nRF
Connect SDK and nrfxlib (LicenseRef-Nordic-5-Clause / Apache-2.0), and MCUboot
(Apache-2.0). See the NOTICE file for the full list. The grant below does not
convey any rights in those third-party components; nothing here permits you to
use, copy, sublicense, or sell them beyond what their own licenses allow.

------------------------------------------------------------------------------

MIT License

Copyright (c) 2026 Benjamin Reece

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Zephyr Project

<https://www.zephyrproject.org>

The firmware is built on Zephyr, which is licensed under the Apache License 2.0. The files in `zephyr-patches/` modify Zephyr v4.3.1 sources (`drivers/usb/udc/udc_nrf.c`, `subsys/usb/device_next/class/usbd_uac2_macros.h` and related files) and are therefore distributed under the Apache License 2.0: <https://www.apache.org/licenses/LICENSE-2.0>.

Zephyr itself is fetched by `west update` and is not included in this repository.

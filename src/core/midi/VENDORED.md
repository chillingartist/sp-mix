# Vendored MIDI core

Copied unchanged from the MIDI core of a feldd-based SP-1 firmware, commit `88bf82695137eecf9f84ff17e459d3b94c7e8e3b`.

| Source file | Destination | SHA-256 |
| --- | --- | --- |
| `app/src/midi_msg.h` | `src/core/midi/midi_msg.h` | `62fd371671db40c9570cb169afd65abf86e149117f67ae2286402c55e60bb545` |
| `app/src/midi_hr.h` | `src/core/midi/midi_hr.h` | `045d07bb3986d91c57a25fd0259036b3af85300db493e35b09dd911220bb7c99` |
| `app/src/midi_hr.c` | `src/core/midi/midi_hr.c` | `b161de7edf3c793f690e536050c1efa8a84a38a64eed0e082d8085b24c7ee33f` |
| `app/src/midi_bus.h` | `src/core/midi/midi_bus.h` | `18475b6ad7aa47dd6429bf6511695cd7ba0aec64bdd52ea749879ee597a97a1f` |
| `app/src/midi_bus.c` | `src/core/midi/midi_bus.c` | `dee3ef2db7f3f286b7ae203693529a6c60c4917560b1a245cd315c854611febb` |
| `app/src/midi_assign.h` | `src/core/midi/midi_assign.h` | `2ffd5daeeb0b471fb0a1967ba319e26fbc0321bfe8d3bcc36e9cf153dcd5b680` |
| `app/src/midi_assign.c` | `src/core/midi/midi_assign.c` | `4794bce709348e49978f9b1e74f3cd6679a49c8ba54fe1508df457be791bac0e` |
| `app/src/midi_params.h` | `src/core/midi/midi_params.h` | `96f144a3467717f9134245f3a8750e2c81b44d69833c44003c706d001590a637` |
| `app/src/midi_params.c` | `src/core/midi/midi_params.c` | `e920f675bad9d14769508d9ef23d54e7c1c789875a31debf76232a6300bbf4e4` |
| `app/src/usb_rt_parse.h` | `src/core/midi/usb_rt_parse.h` | `6d62e41b72256cc0531dbc3978fda7c2f5f8c519f64c0374726fe79851cf2aae` |
| `app/src/usb_rt_parse.c` | `src/core/midi/usb_rt_parse.c` | `a32229d6fcf2bcf0d9f4063440dbc23b232988d1c1bd97dde81a93316ef9af28` |
| `app/src/usb_midi1.h` | `src/platform/usb_midi1.h` | `12db01e5c9ffb75dd1e92584b51485ac8ce80aa28701ef19c182d346550181c4` |
| `app/src/usb_midi1.c` | `src/platform/usb_midi1.c` | `49a6c80ca816f44e04ab539f83de63fe60c24b95f8a2657b1bb4f78c40512b4e` |
| `test/test_midi_hr.c` | `tests/host/midi/test_midi_hr.c` | `ee104ee6cf989715b8b9af205b76a1315613791d67ec3b204836996c59eff8bb` |
| `test/test_midi_bus.c` | `tests/host/midi/test_midi_bus.c` | `2d76c58293f8cab41cda09504343958b849849b67905987e2fda29681791f54f` |
| `test/test_midi_assign.c` | `tests/host/midi/test_midi_assign.c` | `b565d2b14c265f5c203b2b3cfde408d7267d17325bb1cccc91eabf917ccb2c89` |
| `test/test_midi_params.c` | `tests/host/midi/test_midi_params.c` | `a35e80358a42d5c91faed6210e170f1facc9d504b00ff5f25a1cf15ae0db4e77` |
| `test/test_usb_rt_parse.c` | `tests/host/midi/test_usb_rt_parse.c` | `368ac4d3814e80b12b9d99b35b221cc876923c501096605b50bebf96e4baf302` |

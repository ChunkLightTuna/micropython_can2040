# micropython_can2040

MicroPython bindings for [can2040](https://github.com/KevinOConnor/can2040), the software
CAN bus implementation for the RP2040 / RP2350 PIO. Gives a plain Raspberry Pi Pico a CAN 2.0B
interface using only a CAN transceiver chip.

```
micropython_can2040/
├── can2040/       upstream can2040 (git submodule, pinned; GPLv3)
├── modcan2040/    the MicroPython user C module
│   ├── modcan2040.c
│   ├── micropython.cmake
│   ├── can2040_mp_hooks.h
│   └── patches/   Windows build fix applied to the MicroPython tree by build.ps1
├── micropython/   MicroPython v1.29.0 source (not committed; cloned by hand, see below)
├── build.ps1      Windows build + flash script
└── firmware/      built .uf2 files
```

Clone with `git clone --recursive`, or run `git submodule update --init` after a plain clone.
To move to a newer can2040: `git submodule update --remote can2040`, rebuild, commit the new pin.

License: GPLv3, the same as can2040 (see COPYING).

## Python API

```python
import can2040

can = can2040.CAN2040(pio=0, rx=5, tx=4, bitrate=500000)
can.start()

# polling
frame = can.recv()            # (id, data: bytes, rtr: bool, ext: bool) or None
n = can.any()                 # frames waiting in the receive queue (up to 64)

# or a callback, delivered through the MicroPython scheduler (not IRQ context)
def on_rx(can_id, data, rtr, ext):
    print(hex(can_id), data)
can.on_receive(on_rx)         # on_receive(None) clears it

ok = can.send(0x123, b"\x01\x02")             # True if queued (4-deep transmit queue)
ok = can.send(0x1ABCDEF, b"", ext=True, rtr=True)
can.check_transmit()          # True if send() would succeed
can.stats()                   # {'rx':..,'tx':..,'tx_attempts':..,'parse_errors':..,'rx_dropped':..,'rx_overflow':..}
can.running                   # bool
can.stop()                    # releases the PIO block and IRQ
```

Notes:

- `pio` selects PIO block 0 or 1 (2 on RP2350). can2040 uses all four state machines of the block,
  so `rp2.StateMachine` cannot use that block while CAN is running. One `CAN2040` object exists per
  block; constructing it again reconfigures (and stops) the existing one.
- `tx=-1` gives a listen-only sniffer that never acknowledges frames.
- `bitrate` is 10 kbit/s to 1 Mbit/s. The system clock is read at `start()`, so call
  `machine.freq()` before starting CAN if you change it.
- A CAN node needs at least one other node on the bus to acknowledge frames. Sending alone will
  retry forever and `stats()['tx_attempts']` will climb.
- Soft reset (Ctrl-D) stops CAN and releases the hardware automatically.
- The can2040 core is linked into RAM (`CAN2040_IN_RAM=ON`, ~10 KB) so flash cache misses cannot
  disturb its interrupt timing. Pass `-NoRam` to the build script to keep it in flash.

## Building (Windows)

One-time setup:

```powershell
winget install Arm.GnuArmEmbeddedToolchain Kitware.CMake Ninja-build.Ninja
pip install mpy-cross==1.29.0.post2 mpremote
# picotool comes from the Raspberry Pi Pico VS Code extension (%USERPROFILE%\.pico-sdk\picotool)
git clone --depth 1 --branch v1.29.0 https://github.com/micropython/micropython.git
cd micropython
git submodule update --init --depth 1 lib/pico-sdk lib/tinyusb lib/micropython-lib lib/mbedtls lib/lwip lib/cyw43-driver lib/btstack
```

Then:

```powershell
.\build.ps1                      # -> firmware\firmware-RPI_PICO-can2040.uf2
.\build.ps1 -Board RPI_PICO_W    # -> firmware\firmware-RPI_PICO_W-can2040.uf2
.\build.ps1 -Flash               # also reboots the attached Pico into BOOTSEL and copies the uf2
```

The Pico W build additionally needs the `lib/cyw43-driver` and `lib/btstack` submodules and
a prebuilt `pioasm` (the wireless SPI driver ships a `.pio` program). Unpack
`pico-sdk-tools-<sdk version>-x64-win.zip` from
https://github.com/raspberrypi/pico-sdk-tools/releases into
`%USERPROFILE%\.pico-sdk\tools\<sdk version>\`; `build.ps1` picks it up from there.

On the Pico W the wireless driver claims one PIO state machine at boot, so use `pio=1`
(or try both) when the default block is reported as in use.

The mpy-cross version must match the MicroPython source version (both 1.29.0 here).
The script converts all paths to 8.3 short names because the pico-sdk build breaks on
paths with spaces, puts Git's `usr\bin` on the PATH for `touch`/`cat`, and applies
`modcan2040\patches\*.patch` to the MicroPython tree (one patch replaces a `sed` pipeline in
`py/mkrules.cmake` that cmd.exe cannot run). Nothing else in the MicroPython tree is modified.

On Linux/macOS the equivalent is:

```sh
cd micropython/ports/rp2
cmake -S . -B build -G Ninja -DMICROPY_BOARD=RPI_PICO \
      -DUSER_C_MODULES=/abs/path/to/modcan2040/micropython.cmake
ninja -C build
```

## Flashing by hand

Hold BOOTSEL while plugging the Pico in, then copy `firmware\firmware-RPI_PICO-can2040.uf2` onto
the `RPI-RP2` drive. The MicroPython filesystem (your `.py` files) survives a firmware update.

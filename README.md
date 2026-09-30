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
│   └── patches/   Windows build fixes applied to the MicroPython tree by build.py
├── micropython/   MicroPython v1.29.0 source (not committed; cloned by hand, see below)
├── build.py       build + flash script (Windows, macOS, Linux)
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
  disturb its interrupt timing. Pass `--no-ram` to the build script to keep it in flash.

## Building

One-time setup. Tools: `arm-none-eabi-gcc`, CMake, Ninja, Python 3 with `mpy-cross` and
`mpremote`, and prebuilt `picotool` and `pioasm` from
https://github.com/raspberrypi/pico-sdk-tools/releases (the Pico VS Code extension installs
the same files).

```sh
# Windows: winget install Arm.GnuArmEmbeddedToolchain Kitware.CMake Ninja-build.Ninja
# Debian/Ubuntu: apt install gcc-arm-none-eabi cmake ninja-build
# macOS: brew install --cask gcc-arm-embedded; brew install cmake ninja
pip install mpy-cross==1.29.0.post2 mpremote
# unpack pico-sdk-tools-<sdk version>-<os>.zip so that these exist:
#   ~/.pico-sdk/picotool/<ver>/picotool/picotoolConfig.cmake
#   ~/.pico-sdk/tools/<ver>/pioasm/pioasmConfig.cmake        (Pico W builds only)
git clone --depth 1 --branch v1.29.0 https://github.com/micropython/micropython.git
cd micropython
git submodule update --init --depth 1 lib/pico-sdk lib/tinyusb lib/micropython-lib lib/mbedtls lib/lwip lib/cyw43-driver lib/btstack
```

Then:

```sh
python build.py                       # -> firmware/firmware-RPI_PICO-can2040.uf2
python build.py --board RPI_PICO_W    # -> firmware/firmware-RPI_PICO_W-can2040.uf2
python build.py --flash               # also reboots the attached Pico into BOOTSEL and copies the uf2
python build.py --clean               # wipe the build directory first; --no-ram keeps can2040 in flash
```

The script finds the tools on PATH or in their usual install locations, applies
`modcan2040/patches/*.patch` to the MicroPython tree (idempotently), configures with CMake
and builds with Ninja. The mpy-cross version must match the MicroPython source version
(both 1.29.0 here).

On Windows it also converts every path to its 8.3 short form, because the pico-sdk build
breaks on paths with spaces, and puts Git for Windows' `usr\bin` on the PATH for the
`touch`/`cat` that MicroPython's cmake rules call. The patch replaces a `sed` pipeline in
`py/mkrules.cmake` that cmd.exe cannot run and routes the qstr preprocessing source list
through a response file to stay under the 32 KiB command-line limit. Nothing else in the
MicroPython tree is modified, and the patch is harmless on other platforms.

The Pico W build needs the `lib/cyw43-driver` and `lib/btstack` submodules and the prebuilt
`pioasm` (the wireless SPI driver ships a `.pio` program). On the Pico W the wireless driver
claims one PIO state machine at boot, so use `pio=1` (or try both) when the default block is
reported as in use.

## Flashing by hand

Hold BOOTSEL while plugging the Pico in, then copy `firmware\firmware-RPI_PICO-can2040.uf2` onto
the `RPI-RP2` drive. The MicroPython filesystem (your `.py` files) survives a firmware update.

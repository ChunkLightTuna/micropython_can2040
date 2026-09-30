#!/usr/bin/env python3
"""Build MicroPython for the Raspberry Pi Pico with the can2040 CAN module; optionally flash it.

Expected layout (all siblings of this script):
    micropython/   MicroPython source (v1.29.0 with its lib/ submodules)
    can2040/       upstream can2040 (git submodule)
    modcan2040/    the MicroPython C binding (micropython.cmake + modcan2040.c + patches/)

Tools: arm-none-eabi-gcc, CMake, Ninja, Python 3 with mpy-cross (and mpremote for --flash),
a prebuilt picotool (%USERPROFILE%/.pico-sdk/picotool/<ver>/picotool, from the Pico VS Code
extension or github.com/raspberrypi/pico-sdk-tools) and, for the Pico W, a prebuilt pioasm
(%USERPROFILE%/.pico-sdk/tools/<ver>/pioasm from the same pico-sdk-tools release).

    python build.py                       # -> firmware/firmware-RPI_PICO-can2040.uf2
    python build.py --board RPI_PICO_W
    python build.py --flash               # build, reboot the attached Pico into BOOTSEL, copy the uf2
    python build.py --clean               # wipe the build directory first

On Windows every path is converted to its 8.3 short form, because the pico-sdk build does
not cope with spaces in paths, and Git for Windows' usr/bin is added to PATH for the
`touch`/`cat` that MicroPython's cmake rules call.
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

WINDOWS = os.name == "nt"
HOME = Path.home()


def fail(msg):
    sys.exit("error: " + msg)


def short(path):
    """8.3 short path on Windows (the path must exist); unchanged elsewhere."""
    path = str(path)
    if not WINDOWS:
        return path
    import ctypes
    buf = ctypes.create_unicode_buffer(1024)
    if ctypes.windll.kernel32.GetShortPathNameW(path, buf, 1024) == 0:
        fail("cannot get a short path for %s (does it exist?)" % path)
    return buf.value


def newest(pattern):
    hits = sorted(glob.glob(str(pattern)))
    return hits[-1] if hits else None


def find_tool(name, *extra_globs):
    exe = shutil.which(name)
    if exe:
        return exe
    for g in extra_globs:
        hit = newest(g)
        if hit:
            return hit
    return None


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd))
    r = subprocess.run([str(c) for c in cmd], **kw)
    if r.returncode != 0:
        fail("%s failed (exit %d)" % (Path(cmd[0]).name, r.returncode))
    return r


def find_boot_drive():
    candidates = ["%s:\\" % d for d in "DEFGHIJKLMNOPQRSTUVWXYZ"] if WINDOWS else \
        [p for base in ("/Volumes", "/media", "/run/media", "/mnt") for p in glob.glob(base + "/*") + glob.glob(base + "/*/*")]
    for c in candidates:
        try:
            if os.path.isfile(os.path.join(c, "INFO_UF2.TXT")):
                return c
        except OSError:
            pass
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", default="RPI_PICO", help="MicroPython rp2 board name (default RPI_PICO)")
    ap.add_argument("--clean", action="store_true", help="delete the build directory first")
    ap.add_argument("--flash", action="store_true", help="flash the result to the attached Pico")
    ap.add_argument("--no-ram", action="store_true", help="keep the can2040 core in flash instead of RAM")
    ap.add_argument("--port", default=None, help="serial port for --flash (default: mpremote auto)")
    args = ap.parse_args()
    sys.stdout.reconfigure(line_buffering=True)

    root = Path(short(Path(__file__).resolve().parent))
    mpy = root / "micropython"
    build = root / ("build-" + args.board)
    out_dir = root / "firmware"
    for d in (mpy / "ports" / "rp2", root / "can2040" / "src", root / "modcan2040"):
        if not d.is_dir():
            fail("missing directory: %s" % d)

    # --- tools -------------------------------------------------------------------
    gcc = find_tool("arm-none-eabi-gcc",
                    r"C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\*\bin\arm-none-eabi-gcc.exe",
                    r"C:\Program Files\Arm GNU Toolchain arm-none-eabi\*\bin\arm-none-eabi-gcc.exe",
                    HOME / ".pico-sdk" / "toolchain" / "*" / "bin" / "arm-none-eabi-gcc*")
    if not gcc:
        fail("arm-none-eabi-gcc not found (winget install Arm.GnuArmEmbeddedToolchain, or apt install gcc-arm-none-eabi)")
    gcc_bin = short(Path(gcc).parent)

    cmake = find_tool("cmake", r"C:\Program Files\CMake\bin\cmake.exe", HOME / ".pico-sdk" / "cmake" / "*" / "bin" / "cmake*")
    if not cmake:
        fail("cmake not found (winget install Kitware.CMake)")
    ninja = find_tool("ninja",
                      str(HOME / "AppData" / "Local" / "Microsoft" / "WinGet" / "Packages" / "Ninja-build.Ninja*" / "ninja.exe"),
                      HOME / ".pico-sdk" / "ninja" / "*" / "ninja*")
    if not ninja:
        fail("ninja not found (winget install Ninja-build.Ninja)")

    try:
        import mpy_cross
        mpy_cross_exe = mpy_cross.mpy_cross
    except ImportError:
        fail("mpy-cross not installed (pip install mpy-cross==1.29.0.post2; must match the MicroPython version)")

    picotool_cfg = newest(HOME / ".pico-sdk" / "picotool" / "*" / "picotool" / "picotoolConfig.cmake")
    if not picotool_cfg:
        fail("picotool not found under %s (unpack pico-sdk-tools-<ver>-<os>.zip there)" % (HOME / ".pico-sdk" / "picotool"))
    pioasm_cfg = newest(HOME / ".pico-sdk" / "tools" / "*" / "pioasm" / "pioasmConfig.cmake")
    if not pioasm_cfg and args.board.endswith("_W"):
        fail("pioasm not found under %s (needed for %s)" % (HOME / ".pico-sdk" / "tools", args.board))

    env = dict(os.environ)
    path_parts = [gcc_bin, short(Path(ninja).parent), short(Path(cmake).parent), env.get("PATH", "")]
    if WINDOWS:
        git_usr_bin = next((p for p in (r"C:\Program Files\Git\usr\bin", HOME / "AppData" / "Local" / "Programs" / "Git" / "usr" / "bin")
                            if os.path.isfile(os.path.join(p, "touch.exe"))), None)
        if not git_usr_bin:
            fail("Git for Windows usr\\bin not found (needed for touch/cat)")
        path_parts.append(short(git_usr_bin))
    env["PATH"] = os.pathsep.join(str(p) for p in path_parts)
    env["PICO_TOOLCHAIN_PATH"] = gcc_bin
    env["MICROPY_MPYCROSS"] = short(mpy_cross_exe)   # mkrules.cmake and makemanifest.py read it from the environment

    # --- patches for building on Windows (idempotent) ---------------------------------
    for patch in sorted((root / "modcan2040" / "patches").glob("*.patch")):
        applied = subprocess.run(["git", "apply", "--check", "--reverse", str(patch)], cwd=mpy,
                                 capture_output=True).returncode == 0
        if not applied:
            run(["git", "apply", str(patch)], cwd=mpy)
            print("applied", patch.name)

    # --- configure + build --------------------------------------------------------
    if args.clean and build.exists():
        shutil.rmtree(build)
    cmake_args = [
        cmake, "-S", mpy / "ports" / "rp2", "-B", build, "-G", "Ninja",
        "-DMICROPY_BOARD=" + args.board,
        "-DUSER_C_MODULES=" + str(root / "modcan2040" / "micropython.cmake"),
        "-DCAN2040_DIR=" + str(root / "can2040"),
        "-DCAN2040_IN_RAM=" + ("OFF" if args.no_ram else "ON"),
        "-DMICROPY_MPYCROSS=" + env["MICROPY_MPYCROSS"],
        "-Dpicotool_DIR=" + short(Path(picotool_cfg).parent),
        "-DPython3_EXECUTABLE=" + short(sys.executable),
    ]
    if pioasm_cfg:
        cmake_args.append("-Dpioasm_DIR=" + short(Path(pioasm_cfg).parent))
    run(cmake_args, env=env)
    run([ninja, "-C", build], env=env)

    out_dir.mkdir(exist_ok=True)
    uf2 = out_dir / ("firmware-%s-can2040.uf2" % args.board)
    shutil.copyfile(build / "firmware.uf2", uf2)
    print("Firmware:", uf2)

    # --- flash ---------------------------------------------------------------------
    if args.flash:
        print("Rebooting the Pico into BOOTSEL mode...")
        cmd = [sys.executable, "-m", "mpremote"] + (["connect", args.port] if args.port else []) + ["bootloader"]
        subprocess.run(cmd)
        drive = None
        for _ in range(60):
            drive = find_boot_drive()
            if drive:
                break
            time.sleep(1)
        if not drive:
            fail("RPI-RP2 drive did not appear. Hold BOOTSEL while plugging the Pico in, then copy %s to it." % uf2)
        shutil.copyfile(uf2, os.path.join(drive, uf2.name))
        print("Copied to %s. The Pico reboots into the new firmware." % drive)


if __name__ == "__main__":
    main()

<#
.SYNOPSIS
    Build MicroPython for the Raspberry Pi Pico with the can2040 CAN module, and optionally flash it.

.DESCRIPTION
    Expects this layout (all siblings of this script):
        micropython/   MicroPython source (v1.29.0, with lib/pico-sdk etc. submodules)
        can2040/       upstream can2040 checkout (KevinOConnor/can2040)
        modcan2040/    the MicroPython C binding (micropython.cmake + modcan2040.c)

    Tools (installed with winget / pip):
        Arm GNU Toolchain (arm-none-eabi-gcc), CMake, Ninja, Python 3 with mpy-cross and mpremote,
        picotool in %USERPROFILE%\.pico-sdk\picotool\<ver>\picotool (from the Pico VS Code extension).

    All paths are converted to 8.3 short names because the pico-sdk build does not
    cope with spaces in paths.

.EXAMPLE
    .\build.ps1              # builds firmware\firmware-RPI_PICO-can2040.uf2
.EXAMPLE
    .\build.ps1 -Flash       # build, reboot the attached Pico into BOOTSEL, copy the uf2
.EXAMPLE
    .\build.ps1 -Clean       # wipe the build directory first
#>
param(
    [string]$Board = "RPI_PICO",
    [switch]$Clean,
    [switch]$Flash,
    [switch]$NoRam,          # keep the can2040 core in flash instead of RAM
    [string]$Port = "auto"   # serial port for -Flash (e.g. COM6); "auto" lets mpremote pick
)
$ErrorActionPreference = "Stop"

$fso = New-Object -ComObject Scripting.FileSystemObject
function ShortDir([string]$p)  { $fso.GetFolder($p).ShortPath }
function ShortFile([string]$p) { $fso.GetFile($p).ShortPath }

$root   = ShortDir (Split-Path -Parent $MyInvocation.MyCommand.Path)
$mpy    = "$root\micropython"
$build  = "$root\build-$Board"
$outDir = "$root\firmware"

foreach ($d in @("$mpy\ports\rp2", "$root\can2040\src", "$root\modcan2040")) {
    if (-not (Test-Path $d)) { throw "Missing directory: $d" }
}

# --- locate tools -------------------------------------------------------------
$gccBin = Get-ChildItem "C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\*\bin",
                        "C:\Program Files\Arm GNU Toolchain arm-none-eabi\*\bin" -ErrorAction SilentlyContinue |
          Sort-Object FullName -Descending | Select-Object -First 1
if (-not $gccBin) { throw "Arm GNU Toolchain not found (winget install Arm.GnuArmEmbeddedToolchain)" }
$gccBin = ShortDir $gccBin.FullName

$cmakeExe = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmakeExe) { $cmakeExe = "C:\Program Files\CMake\bin\cmake.exe" }
if (-not (Test-Path $cmakeExe)) { throw "cmake not found (winget install Kitware.CMake)" }
$cmakeExe = ShortFile $cmakeExe

$ninjaExe = (Get-Command ninja -ErrorAction SilentlyContinue).Source
if (-not $ninjaExe) {
    $ninjaExe = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Ninja-build.Ninja*\ninja.exe" -ErrorAction SilentlyContinue |
                 Select-Object -First 1).FullName
}
if (-not $ninjaExe) { throw "ninja not found (winget install Ninja-build.Ninja)" }
$ninjaExe = ShortFile $ninjaExe

$pythonExe = & python -c "import sys; print(sys.executable)"
if (-not $pythonExe) { throw "python not found" }
$pythonExe = ShortFile $pythonExe
$mpyCross = & $pythonExe -c "import mpy_cross; print(mpy_cross.mpy_cross)"
if (-not $mpyCross) { throw "mpy-cross not found (pip install mpy-cross==1.29.0.post2)" }
$mpyCross = ShortFile $mpyCross

$picotool = Get-ChildItem "$env:USERPROFILE\.pico-sdk\picotool\*\picotool\picotoolConfig.cmake" -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1
if (-not $picotool) { throw "picotool not found under $env:USERPROFILE\.pico-sdk\picotool" }
$picotoolDir = ShortDir $picotool.DirectoryName

# pioasm is only needed by boards whose SDK libraries carry .pio programs (Pico W: cyw43 SPI).
# Prebuilt: pico-sdk-tools-<ver>-x64-win.zip from github.com/raspberrypi/pico-sdk-tools,
# unpacked to %USERPROFILE%\.pico-sdk\tools\<ver>\ (same layout as the Pico VS Code extension).
$pioasm = Get-ChildItem "$env:USERPROFILE\.pico-sdk\tools\*\pioasm\pioasmConfig.cmake" -ErrorAction SilentlyContinue |
          Sort-Object FullName -Descending | Select-Object -First 1
$pioasmArg = @()
if ($pioasm) { $pioasmArg = @("-Dpioasm_DIR=$(ShortDir $pioasm.DirectoryName)") }
elseif ($Board -match "_W$") { throw "pioasm not found under $env:USERPROFILE\.pico-sdk\tools (needed for $Board)" }

# MicroPython's cmake rules shell out to `touch`, `cat` and `git`; Git for Windows provides them.
$gitUsrBin = @("C:\Program Files\Git\usr\bin", "$env:LOCALAPPDATA\Programs\Git\usr\bin") |
             Where-Object { Test-Path "$_\touch.exe" } | Select-Object -First 1
if (-not $gitUsrBin) { throw "Git for Windows usr\bin not found (needed for touch/cat)" }
$gitUsrBin = ShortDir $gitUsrBin

$env:Path = "$gccBin;$(Split-Path $ninjaExe);$(Split-Path $cmakeExe);$env:Path;$gitUsrBin"
$env:PICO_TOOLCHAIN_PATH = $gccBin
# mkrules.cmake and makemanifest.py read mpy-cross from the environment, not from -D.
$env:MICROPY_MPYCROSS = $mpyCross

# --- apply Windows build patches to the MicroPython tree -----------------------
# (idempotent: skipped when a patch is already applied)
foreach ($patch in Get-ChildItem "$root\modcan2040\patches\*.patch" -ErrorAction SilentlyContinue) {
    Push-Location $mpy
    try {
        & git apply --check --reverse $patch.FullName 2>$null
        if ($LASTEXITCODE -ne 0) {
            & git apply $patch.FullName
            if ($LASTEXITCODE -ne 0) { throw "failed to apply $($patch.Name)" }
            Write-Host "Applied $($patch.Name)"
        }
    } finally { Pop-Location }
}

# --- configure + build --------------------------------------------------------
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

$ramFlag = if ($NoRam) { "OFF" } else { "ON" }
& $cmakeExe -S "$mpy\ports\rp2" -B $build -G Ninja `
    "-DMICROPY_BOARD=$Board" `
    "-DUSER_C_MODULES=$root\modcan2040\micropython.cmake" `
    "-DCAN2040_DIR=$root\can2040" `
    "-DCAN2040_IN_RAM=$ramFlag" `
    "-DMICROPY_MPYCROSS=$mpyCross" `
    "-Dpicotool_DIR=$picotoolDir" `
    "-DPython3_EXECUTABLE=$pythonExe" `
    @pioasmArg
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

& $ninjaExe -C $build
if ($LASTEXITCODE -ne 0) { throw "build failed" }

New-Item -ItemType Directory -Force $outDir | Out-Null
$uf2 = "$outDir\firmware-$Board-can2040.uf2"
Copy-Item "$build\firmware.uf2" $uf2 -Force
Write-Host "Firmware: $uf2"

# --- flash ---------------------------------------------------------------------
if ($Flash) {
    Write-Host "Rebooting the Pico into BOOTSEL mode..."
    if ($Port -eq "auto") { & $pythonExe -m mpremote bootloader } else { & $pythonExe -m mpremote connect $Port bootloader }
    $drive = $null
    for ($i = 0; $i -lt 60 -and -not $drive; $i++) {
        Start-Sleep -Seconds 1
        $drive = Get-CimInstance Win32_LogicalDisk | Where-Object { $_.VolumeName -eq "RPI-RP2" } | Select-Object -First 1
    }
    if (-not $drive) { throw "RPI-RP2 drive did not appear. Hold BOOTSEL while plugging the Pico in, then copy $uf2 to it." }
    Copy-Item $uf2 "$($drive.DeviceID)\" -Force
    Write-Host "Copied to $($drive.DeviceID). The Pico reboots into the new firmware."
}

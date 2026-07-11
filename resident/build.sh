#!/bin/bash
# M6.2a resident-vehicle build: SiI3512SATA.pef (main cmake, now w/ InstallMe) -> 68K INIT .flt
# (m68k toolchain) -> Rez packs both into a locked 'INIT' extension file (SiI3512Init).
# Two toolchains; mirrors usb2-ehci/resident/build.sh. See reference_os9_init_resident_driver.
# TEST ON THE OS9 LAB VOLUME (boot-code safety); recover a bad boot via Shift-boot + remove.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"        # .../esata-sil3512/resident
ROOT="$(cd "$HERE/.." && pwd)"               # .../esata-sil3512
RB="$HOME/Retro68-build"
M68KGCC="$RB/toolchain-m68k/bin/m68k-apple-macos-gcc"
PPCBIN="$RB/toolchain/bin"
REZ="$PPCBIN/Rez"
RINC="$RB/toolchain-m68k/m68k-apple-macos/RIncludes"
LIBRETRO_R="$(dirname "$(find "$HOME/Retro68" "$RB" -name Retro68.r 2>/dev/null | head -1)")"
BD="$HERE/build"; mkdir -p "$BD"

echo "=== 1. SiI3512SATA.pef (PPC driver fragment w/ InstallMe) via main cmake ==="
export PATH="$PPCBIN:$PATH"
cmake --build "$ROOT/build" 2>&1 | tail -4
cp "$ROOT/build/SiI3512SATA.pef" "$BD/SiI3512SATA.pef"
ls -l "$BD/SiI3512SATA.pef"

echo "=== 2. esata_init.flt (68K INIT code resource) ==="
"$M68KGCC" -Wno-multichar -O2 -Wl,--mac-flat "$HERE/esata_init.c" -o "$BD/esata_init.flt"
ls -l "$BD/esata_init.flt"

echo "=== 3. Rez -> SiI3512Init extension (type INIT + embedded 'PPC ' driver PEF) ==="
cp "$HERE/esata_init.r" "$BD/esata_init.r"
( cd "$BD" && "$REZ" -I"$LIBRETRO_R" -I"$RINC" esata_init.r \
    -o SiI3512Init.bin --cc SiI3512Init.dsk --cc SiI3512Init -t INIT -c RSED )
echo "=== done ==="; ls -l "$BD"/SiI3512Init*

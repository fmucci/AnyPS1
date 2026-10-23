#!/usr/bin/env bash
# Builds the reference emulator harness under work/emu/ (git-ignored; nothing of it is ever committed or redistributed):
#   1. clone PCSX-ReARMed (libretro port, GPL) 2. add our debug instrumentation (patch_core.py) 3. build the core as a shared library (interpreter, built-in HLE BIOS: no BIOS image needed)
#   4. compile our own headless libretro host (tools/ref_emu/host.c).
# usage: tools/ref_emu/setup_ref.sh        needs: git, make, a C compiler. Idempotent. Output: work/emu/pcsx_rearmed/pcsx_rearmed_libretro.{dylib,so,dll}, work/emu/host/host2
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
E="$ROOT/work/emu"; mkdir -p "$E/host"; cd "$E"
if [ ! -d pcsx_rearmed ]; then
  git clone --depth 1 https://github.com/libretro/pcsx_rearmed
  (cd pcsx_rearmed && git submodule update --init --depth 1)
fi
python3 -I "$ROOT/tools/ref_emu/patch_core.py" "$E/pcsx_rearmed"
cd pcsx_rearmed
case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) PLAT="platform=osx ARCH=arm64" ;;
  Darwin-x86_64) PLAT="platform=osx ARCH=x86_64" ;;
  Linux-aarch64) PLAT="ARCH=arm64" ;;
  *) PLAT="" ;;
esac
make -f Makefile.libretro $PLAT DYNAREC=0 -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
cc -O1 -o "$E/host/host2" "$ROOT/tools/ref_emu/host.c" -ldl
echo "reference harness ready: $E/host/host2 + $(ls "$E"/pcsx_rearmed/pcsx_rearmed_libretro.* | head -1)"

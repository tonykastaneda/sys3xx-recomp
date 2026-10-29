#!/usr/bin/env bash
# Lift GAME_208.elf (Bloodline Rebellion) to recompiled_208/ (PPU). The --hook functions are
# replaced natively in port/t6_patches.cpp (USB key check).
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"; cd "$root"
.venv/Scripts/python.exe ps3recomp/tools/ppu_loader.py elf/GAME_208.elf -o out/g208/ > /dev/null
rm -rf recompiled_208
.venv/Scripts/python.exe ps3recomp/tools/ppu_lifter.py elf/GAME_208.elf \
  --functions out/g208/GAME_208.functions.json --hle-stubs out/g208/GAME_208.imports.json \
  --code-end 0x59e87c --nonvolatile-locals --chunks 16 \
  --hook 0x0014C2C8 --hook 0x0014BF90 \
  -o recompiled_208/ > out/g208/lift.log 2>&1
tail -3 out/g208/lift.log

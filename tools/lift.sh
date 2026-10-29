#!/usr/bin/env bash
# Lift GAME_113.elf to recompiled_113/ (PPU). Hooks = functions supplied natively in port/t6_patches.cpp.
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"; cd "$root"
.venv/Scripts/python.exe ps3recomp/tools/ppu_loader.py elf/GAME_113.elf -o out/g113/ > /dev/null
rm -rf recompiled_113
.venv/Scripts/python.exe ps3recomp/tools/ppu_lifter.py elf/GAME_113.elf \
  --functions out/g113/GAME_113.functions.json --hle-stubs out/g113/GAME_113.imports.json \
  --code-end 0x6639dc --nonvolatile-locals --chunks 16 \
  --hook 0x0015EB20 --hook 0x0015EE78 \
  -o recompiled_113/ > out/g113/lift.log 2>&1
tail -3 out/g113/lift.log

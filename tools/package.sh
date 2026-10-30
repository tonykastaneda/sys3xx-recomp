#!/usr/bin/env bash
# Assemble dist/T6BR: t6br.exe + config.exe + the FFmpeg DLLs the movie decoder loads + README.
# The game dump is NOT copied (disk space): t6.ini's [Paths] Game= points at it.
#   tools/package.sh [game dir]      (default: the repo's game/)
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"; out="$root/dist/T6BR"
game="$(cygpath -w "${1:-$root/game}")"
mkdir -p "$out"
cp -u "$root/build/t6br.exe" "$root/build/config.exe" "$out/"
for d in avcodec-62 avutil-60 swresample-6; do cp -u "$root/build/$d.dll" "$out/"; done
cp -u "$root/dist_README.txt" "$out/README.txt"
if [ ! -f "$out/t6.ini" ]; then
  printf '[Paths]\r\nGame=%s\r\n' "$game" > "$out/t6.ini"   # t6br.exe fills in the rest on first run
fi
du -sh "$out"; ls "$out"

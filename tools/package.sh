#!/usr/bin/env bash
# Assemble dist/<game>/: <exe>.exe + config.exe + the FFmpeg DLLs the movie decoder loads
# + README. The dump is NOT copied (disk space): <exe>.ini's [Paths] Game= points at it.
#   tools/package.sh <game> [dump dir]      (default: GAME_DUMP from local.env)
set -e
. "$(dirname "$0")/game.sh"
out="$ROOT/dist/$GAME"; b="$WORK/build"
dump="${2:-$GAME_DUMP}"
mkdir -p "$out"
cp -u "$b/$GAME_EXE.exe" "$b/config.exe" "$out/"
for d in avcodec-62 avutil-60 swresample-6; do cp -u "$b/$d.dll" "$out/"; done
[ -f "$GDIR/README.txt" ] && cp -u "$GDIR/README.txt" "$out/README.txt"
if [ ! -f "$out/$GAME_EXE.ini" ] && [ -n "$dump" ]; then
  printf '[Paths]\r\nGame=%s\r\n' "$(cygpath -w "$dump")" > "$out/$GAME_EXE.ini"   # the exe fills in the rest
fi
du -sh "$out"; ls "$out"

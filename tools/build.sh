#!/usr/bin/env bash
# Configure + build a game with clang-cl + xwin (no Visual Studio), plus its config.exe.
#   tools/build.sh <game>            configure (if needed) + build  -> work/<game>/build/<exe>.exe
#   tools/build.sh <game> clean      wipe the build dir first
# Needs the lifted code (tools/lift.sh <game>, tools/lift_spu.sh <game>).
set -e
. "$(dirname "$0")/game.sh"
root="$(cygpath -m "$ROOT")"
export PATH="$HOME/tools/clangcl/bin:$HOME/scoop/apps/mingw-mstorsjo-llvm-msvcrt/current/bin:$PATH"
export MSYS_NO_PATHCONV=1
bd="$(cygpath -m "$WORK/build")"
[ "$2" = clean ] && rm -rf "$bd"
if [ ! -f "$bd/build.ninja" ]; then
  cmake -S "$root/core" -B "$bd" -G Ninja -DCMAKE_BUILD_TYPE=Release -DGAME="$GAME" \
    -DCMAKE_TOOLCHAIN_FILE="$root/cmake/clangcl-xwin.cmake" \
    -DPS3RECOMP_FFMPEG_RUNTIME=ON -DPS3RECOMP_FFMPEG_INCLUDE_DIR="$(cygpath -m ~/tools/ffmpeg/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/include)"
fi
cmake --build "$bd" "${@:3}"
# FFmpeg DLLs (LGPL, loaded at run time by the movie decoder) next to the exe
cp -u ~/tools/ffmpeg/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin/*.dll "$bd/" 2>/dev/null || true
bash "$ROOT/tools/build_config.sh" "$GAME"

#!/usr/bin/env bash
# Configure + build the native port with clang-cl + xwin (no Visual Studio).
#   tools/build.sh            configure (if needed) + build
#   tools/build.sh clean      wipe build dir first
set -e
root="$(cygpath -m "$(cd "$(dirname "$0")/.." && pwd)")"
export PATH="$HOME/tools/clangcl/bin:$HOME/scoop/apps/mingw-mstorsjo-llvm-msvcrt/current/bin:$PATH"
export MSYS_NO_PATHCONV=1
bd="$root/build"
[ "$1" = clean ] && rm -rf "$bd"
if [ ! -f "$bd/build.ninja" ]; then
  cmake -S "$root/port" -B "$bd" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$root/cmake/clangcl-xwin.cmake" \n    -DPS3RECOMP_FFMPEG_RUNTIME=ON -DPS3RECOMP_FFMPEG_INCLUDE_DIR="$(cygpath -m ~/tools/ffmpeg/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/include)"
fi
cmake --build "$bd" "${@:2}"
# FFmpeg DLLs (LGPL, loaded at run time by the movie decoder) next to the exe
cp -u ~/tools/ffmpeg/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin/*.dll "$bd/" 2>/dev/null || true

#!/usr/bin/env bash
# Build a game's config.exe (standalone; no CMake needed) into work/<game>/build/.
#   tools/build_config.sh <game>
set -e
. "$(dirname "$0")/game.sh"
out="$WORK/build"; mkdir -p "$out/cfgobj"
SR=$(cygpath -w ~/tools/xwin/sdk); RD=$(cygpath -m ~/scoop/apps/mingw-mstorsjo-llvm-msvcrt/current/lib/clang/*)
export PATH=~/tools/clangcl/bin:~/scoop/apps/mingw-mstorsjo-llvm-msvcrt/current/bin:$PATH MSYS_NO_PATHCONV=1
clang-cl -nologo -O2 -EHsc -std:c++20 -W3 -D_CRT_SECURE_NO_WARNINGS -DUNICODE -D_UNICODE -winsysroot "$SR" -resource-dir "$RD" \
  -I"$(cygpath -m "$GDIR")" \
  "$(cygpath -m "$ROOT/config/config.cpp")" -Fe:"$(cygpath -m "$out/config.exe")" -Fo:"$(cygpath -m "$out/cfgobj")/" -fuse-ld=lld \
  -link -winsysroot:"$SR" -subsystem:windows user32.lib gdi32.lib shell32.lib comctl32.lib xinput.lib
echo "built $out/config.exe"

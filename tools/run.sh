#!/usr/bin/env bash
# Dev run of work/<game>/build/<exe>.exe against the game's dump (GAME_DUMP in
# games/<game>/local.env). The launcher supplies the game's boot args and switches;
# extra env passes through.
#   tools/run.sh <game> [seconds] [log]
. "$(dirname "$0")/game.sh"
secs="${2:-60}"; log="${3:-$WORK/out/run/latest.log}"; mkdir -p "$(dirname "$log")"
[ -n "$GAME_DUMP" ] || { echo "set GAME_DUMP in $GDIR/local.env" >&2; exit 1; }
need_elf
export PS3_VFS_ROOT="$(cygpath -m "$GAME_DUMP")"
export PS3_HDD0_ROOT="$(cygpath -m "$GAME_DUMP/dev_hdd0")"
[ -n "$DEV_FLASH" ] && export PS3_DEV_FLASH="${PS3_DEV_FLASH:-$DEV_FLASH}"
export MSYS2_ENV_CONV_EXCL="PS3_BOOT_PATH;PS3_ARGV"
export PS3_VERBOSE="${PS3_VERBOSE:-0}"
timeout "$secs" "$WORK/build/$GAME_EXE.exe" "$(cygpath -m "$ELF")" > "$log" 2>&1

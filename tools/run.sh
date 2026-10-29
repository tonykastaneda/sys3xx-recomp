#!/usr/bin/env bash
# Dev run of build/t6.exe against the repo's game/ dump. Extra env passes through.
#   tools/run.sh [seconds] [log]
root="$(cd "$(dirname "$0")/.." && pwd)"
secs="${1:-60}"; log="${2:-$root/out/run/latest.log}"
export PS3_TITLE="TEKKEN 6"
export PS3_VFS_ROOT="$(cygpath -m "$root/game")"
export PS3_HDD0_ROOT="$(cygpath -m "$root/game/dev_hdd0")"
export PS3_DEV_FLASH="${PS3_DEV_FLASH:-E:/SYSTEMS/TEKKNOPAROT/RPCS3/dev_flash}"
export MSYS2_ENV_CONV_EXCL="PS3_BOOT_PATH;PS3_ARGV"
export PS3_BOOT_PATH="/dev_hdd0/game/SCEEXE000/USRDIR/GAME_208.SELF"
export PS3_ARGV="${PS3_ARGV:-ver=206;romver=B23;livemon=0;usbid=026450000000;usbnum=1;idpath=/dev_usb000/T6.ID;datainfo=DINF_004.BIN}"
timeout "$secs" "$root/build/${T6_EXE:-t6}.exe" "$(cygpath -m "$root/elf/GAME_208.elf")" > "$log" 2>&1

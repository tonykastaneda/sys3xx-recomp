#!/usr/bin/env bash
# Lift a game's SPU code into work/<game>/spu/: the embedded SPU thread images, the SPURS job
# binaries (SPU_JOBS in game.env), the workload registry (spu_workloads.c) and the
# job-by-address registry (spu_jobs.c).
#   tools/lift_spu.sh <game>
set -e
. "$(dirname "$0")/game.sh"
need_elf
spu="$WORK/spu"; log="$WORK/out"
rm -rf "$spu"; mkdir -p "$spu/images"
"$PY" "$ROOT/ps3recomp/tools/extract_spu_images.py" "$ELF" --output "$spu/images" > "$log/spu_extract.log" 2>&1
"$PY" "$ROOT/tools/spu_jobs.py" wrap "$ELF" "$spu/images" "$SPU_JOBS"
extra=""
[ "${SPU_TABLE_ENTRIES:-0}" = 1 ] && extra=$("$PY" "$ROOT/tools/spu_jobs.py" table-entries "$spu/images")
[ -n "$extra" ] && echo "extra SPU entries: $extra"
"$PY" "$ROOT/ps3recomp/tools/build_spu_workloads.py" --images "$spu/images" --lifted "$spu" \
    --out "$spu/spu_workloads.c" --register-fn game_spu_register_all --constructor --title "$GAME" \
    --relift $extra > "$log/spu_lift.log" 2>&1
"$PY" "$ROOT/tools/spu_jobs.py" registry "$spu/spu_workloads.c" "$spu/spu_jobs.c"
tail -1 "$log/spu_lift.log"

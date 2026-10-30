#!/usr/bin/env bash
# Lift a game's PPU code to work/<game>/recompiled/. The ELF is extracted from the dump the
# first time. LIFT_HOOKS (game.env) are functions games/<game>/patches.cpp supplies natively.
#   tools/lift.sh <game>
set -e
. "$(dirname "$0")/game.sh"
need_elf
out="$WORK/out/lift"; mkdir -p "$out"
"$PY" "$ROOT/ps3recomp/tools/ppu_loader.py" "$ELF" -o "$out/" > /dev/null
hooks=(); for h in $LIFT_HOOKS; do hooks+=(--hook "$h"); done
rm -rf "$WORK/recompiled"
"$PY" "$ROOT/ps3recomp/tools/ppu_lifter.py" "$ELF" \
  --functions "$out/$GAME_SELF.functions.json" --hle-stubs "$out/$GAME_SELF.imports.json" \
  --code-end "$CODE_END" --nonvolatile-locals --chunks 16 "${hooks[@]}" \
  -o "$WORK/recompiled/" > "$out/lift.log" 2>&1
tail -3 "$out/lift.log"

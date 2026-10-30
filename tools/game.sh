# Sourced by the tools/ scripts: `. tools/game.sh <game>` (a folder under games/).
# Sets ROOT, GAME, GDIR (games/<game>), WORK (work/<game>), ELF, PY and the values of
# games/<game>/game.env, plus GAME_DUMP from games/<game>/local.env:
#
#   games/<game>/local.env (not committed):
#     GAME_DUMP="D:/dumps/Tekken6"      # the extracted dump: the folder that holds dev_hdd0
#     DEV_FLASH="E:/RPCS3/dev_flash"    # optional
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME="${1:?usage: $(basename "$0") <game> ... (games: $(ls "$ROOT/games" | tr '\n' ' '))}"
GDIR="$ROOT/games/$GAME"
[ -f "$GDIR/game.env" ] || { echo "no such game: $GAME (games: $(ls "$ROOT/games" | tr '\n' ' '))" >&2; exit 1; }
. "$GDIR/game.env"
[ -f "$GDIR/local.env" ] && . "$GDIR/local.env"
WORK="$ROOT/work/$GAME"
ELF="$WORK/elf/$GAME_SELF.elf"
PY="$ROOT/.venv/Scripts/python.exe"; [ -x "$PY" ] || PY=python
mkdir -p "$WORK/out"

# The ELF: extracted from the dump's debug SELF the first time.
need_elf() {
  [ -f "$ELF" ] && return 0
  [ -n "$GAME_DUMP" ] || { echo "set GAME_DUMP in $GDIR/local.env (the folder that holds dev_hdd0)" >&2; exit 1; }
  mkdir -p "$WORK/elf"
  "$PY" "$ROOT/tools/dself2elf.py" "$GAME_DUMP/dev_hdd0/game/SCEEXE000/USRDIR/$GAME_SELF.SELF" "$ELF"
}

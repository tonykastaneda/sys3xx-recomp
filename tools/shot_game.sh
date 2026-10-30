#!/usr/bin/env bash
# Wait for the game's window, wait N more seconds, save a PNG of it.
#   tools/shot_game.sh <game> OUT.png [delay]
. "$(dirname "$0")/game.sh"
W="$(cygpath -w "$ROOT/tools/win.ps1")"
t=0; until pwsh -NoProfile -File "$W" list "$GAME_EXE" | grep -q . || [ $t -ge 40 ]; do sleep 2; t=$((t+2)); done
sleep "${3:-20}"
h=$(pwsh -NoProfile -File "$W" list "$GAME_EXE" | head -1 | cut -f1); [ -n "$h" ] && pwsh -NoProfile -File "$W" shot "$h" "$2"

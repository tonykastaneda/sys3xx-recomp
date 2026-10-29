#!/usr/bin/env bash
# Wait for the TEKKEN 6 window, wait N more seconds, save a PNG of it. usage: shot_game.sh OUT.png [delay]
root="$(cd "$(dirname "$0")/.." && pwd)"; W="$(cygpath -w "$root/.claude/hooks/win.ps1")"
t=0; until pwsh -NoProfile -File "$W" list t6 | grep -q . || [ $t -ge 40 ]; do sleep 2; t=$((t+2)); done
sleep "${2:-20}"
h=$(pwsh -NoProfile -File "$W" list t6 | head -1 | cut -f1); [ -n "$h" ] && pwsh -NoProfile -File "$W" shot "$h" "$1"

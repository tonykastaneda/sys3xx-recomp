#!/usr/bin/env bash
# usage: bisect_start.sh "<raw actions without times>" tag  -> screenshot after pressing at the PRESS START screen
root="$(cd "$(dirname "$0")/.." && pwd)"; cd "$root"
acts=""; for a in $1; do for tt in 66 70 74 78; do acts="$acts,$tt:$a:600"; done; done
taskkill //F //IM t6br.exe >/dev/null 2>&1
(T6_INPUT_SCRIPT="60:coin1,62:coin1$acts" bash tools/run.sh 100 out/run/bis_$2.log &)
W="$(cygpath -w .claude/hooks/win.ps1)"; t=0; until pwsh -NoProfile -File "$W" list t6br | grep -q . || [ $t -ge 40 ]; do sleep 2; t=$((t+2)); done
sleep 82; h=$(pwsh -NoProfile -File "$W" list t6br | head -1 | cut -f1); pwsh -NoProfile -File "$W" shot $h "C:\Users\tonyk\AppData\Local\Temp\claude\shots\bis_$2.png" >/dev/null
ls -la /c/Users/tonyk/AppData/Local/Temp/claude/shots/bis_$2.png | awk '{print $5}'

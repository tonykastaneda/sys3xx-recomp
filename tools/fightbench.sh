#!/usr/bin/env bash
# Fight benchmark: coin, start, pick a character, fight; prints fight-time fps,
# SPU waits and torn-buffer counts, then one line: the mean fps over the fight
# (5 s windows from t=85 s on) and the slowest window. Extra env passes through.
#   tools/fightbench.sh [log] [secs]
root="$(cd "$(dirname "$0")/.." && pwd)"
log="${1:-$root/out/run/bench.log}"; secs="${2:-150}"
mkdir -p "$(dirname "$log")"
export PS3_VERBOSE=0 T6_FPS_LOG=1 RSX_WAIT_JOBS_LOG=1 RSX_VCHECK=${RSX_VCHECK:-1}
export T6_INPUT_SCRIPT="${T6_INPUT_SCRIPT:-40:coin1,45:start1,52:lp1,58:lp1,64:lp1,70:lp1,76:lp1,96:right1:1500,98:lp1,99:lk1,100:rp1,101:rk1,115:lp1,116:lk1,130:rp1,131:rk1}"
bash "$root/tools/run.sh" "$secs" "$log"
grep -a -E "frametime|wait-spu|vrace|vcheck|drain thread" "$log" | tail -16
grep -a "\[frametime\]" "$log" | awk -v from=85 '{t=$2; sub("t=","",t); sub("s","",t); if (t+0 >= from) {n++; s+=$3; if (!m || $3 < m) m=$3}} END{if (n) printf "[bench] fight mean %.1f fps over %d windows, slowest %.1f\n", s/n, n, m; else print "[bench] no fight windows"}'

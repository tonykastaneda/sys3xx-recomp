#!/usr/bin/env bash
# Lift T6's SPU code into spu/ (gitignored): the embedded SPU thread images, the SPURS job
# binaries (raw code+data blobs in the game's data segment, wrapped as ELFs at base 0), the
# workload registry (spu_workloads.c) and the job-by-address registry (t6_spu_jobs.c).
#   tools/lift_spu.sh [elf]      default elf/GAME_113.elf
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"; cd "$root"
ELF="${1:-elf/GAME_113.elf}"
PY=.venv/Scripts/python.exe
mkdir -p spu/images out
$PY ps3recomp/tools/extract_spu_images.py "$ELF" --output spu/images > out/spu_extract.log 2>&1
# SPURS job binaries: (guest address, size). Found from the job headers at run time
# (SPU_DUMP_MISS / sizeBinary); GAME_113 layout: data segment 0x10000000 at file 0x730000,
# 0x100C0000 at 0x7F0000.
$PY - "$ELF" <<'EOF'
import subprocess, sys
d = open(sys.argv[1], 'rb').read()
def fo(va): return va - 0x10000000 + 0x730000 if va < 0x100c0000 else va - 0x100c0000 + 0x7f0000
jobs = [(0x1005e280,0x180),(0x1005e400,0x4580),(0x10062980,0x4680),(0x10067000,0x21380),(0x10113880,0x800),
        (0x10114080,0x1100),(0x10115180,0x600),(0x10115780,0x2300),(0x10117a80,0x1f00),(0x10119980,0x1180),
        (0x1011ab00,0x1f00),(0x1011ca00,0x1b00),(0x1011e500,0x1f80),(0x10120480,0x5680),(0x10125b00,0x1b00)]
for va, n in jobs:
    open('spu/job.bin', 'wb').write(d[fo(va):fo(va) + n])
    subprocess.run([sys.executable, 'ps3recomp/tools/wrap_spu_elf.py', 'spu/job.bin', '--base', '0x0',
                    '--entry', '0x0', '--out', f'spu/images/spurs_job_{va:08X}.elf'], check=True)
print('wrapped', len(jobs), 'jobs')
EOF
$PY ps3recomp/tools/build_spu_workloads.py --images spu/images --lifted spu --out spu/spu_workloads.c \
    --register-fn t6_spu_register_all --constructor --title t6 > out/spu_lift.log 2>&1
$PY - <<'EOF'
import re
s = open('spu/spu_workloads.c', encoding='utf-8').read()
rows = re.findall(r'spu_workload_register_img\(0x[0-9A-F]+ULL, (spurs_job_([0-9A-F]{8})_spu_func_\w+), (\d+), "spurs_job_\2"\)', s)
out = ['/* Generated: SPURS job binaries by guest address (see spu_workload_register_ea). */', '#include "spu_context.h"',
       'typedef void (*spu_fn)(spu_context*);', 'extern void spu_workload_register_ea(unsigned int ea, spu_fn fn, int image_id);']
for fn, ea, iid in rows: out.append(f'extern void {fn}(spu_context*);')
out.append('void t6_spu_register_jobs(void)\n{')
for fn, ea, iid in rows: out.append(f'    spu_workload_register_ea(0x{ea}u, {fn}, {iid});')
out.append('}\n__attribute__((constructor)) static void t6_spu_jobs_ctor(void) { t6_spu_register_jobs(); }')
open('spu/t6_spu_jobs.c', 'w').write('\n'.join(out) + '\n')
print('job registrations:', len(rows))
EOF
ls spu | head -30

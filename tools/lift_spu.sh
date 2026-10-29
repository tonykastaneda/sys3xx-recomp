#!/usr/bin/env bash
# Lift BR's SPU code into spu/ (gitignored): the embedded SPU thread images, the SPURS job
# binaries (raw code+data blobs, one per .bin_objs.* section, wrapped as ELFs at base 0), the
# workload registry (spu_workloads.c) and the job-by-address registry (t6_spu_jobs.c).
#   tools/lift_spu.sh [elf]      default elf/GAME_208.elf
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"; cd "$root"
ELF="${1:-elf/GAME_208.elf}"
PY=.venv/Scripts/python.exe
mkdir -p spu/images out
$PY ps3recomp/tools/extract_spu_images.py "$ELF" --output spu/images > out/spu_extract.log 2>&1
# SPURS job binaries: every .bin_objs.<name> section (GAME_208 names them, unlike GAME_113).
$PY - "$ELF" <<'EOF'
import struct, subprocess, sys
d = open(sys.argv[1], 'rb').read()
shoff = struct.unpack('>Q', d[0x28:0x30])[0]; n, si = struct.unpack('>HH', d[0x3c:0x40])
sh = lambda i: struct.unpack('>IIQQQQIIQQ', d[shoff + i * 64:shoff + i * 64 + 64])
strtab = sh(si)[4]
jobs = []
for i in range(n):
    nm, t, f, va, off, size = sh(i)[:6]
    name = d[strtab + nm:d.index(b'\0', strtab + nm)].decode()
    if name.startswith('.bin_objs.'): jobs.append((va, off, size, name[10:]))
for va, off, size, name in jobs:
    open('spu/job.bin', 'wb').write(d[off:off + size])
    subprocess.run([sys.executable, 'ps3recomp/tools/wrap_spu_elf.py', 'spu/job.bin', '--base', '0x0',
                    '--entry', '0x0', '--out', f'spu/images/spurs_job_{va:08X}.elf'], check=True,
                   stdout=subprocess.DEVNULL)
    print(f'  job {va:08X} {size:6X} {name}')
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
ls spu | head -40

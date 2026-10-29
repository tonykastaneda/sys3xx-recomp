"""Find code that loads an address via the TOC (lwz rX, off(r2)) in GAME_113.elf.
usage: xref.py ELF FUNCTIONS_JSON 0xADDR [...]"""
import struct, sys, json, bisect
from capstone import *
elf, fj = sys.argv[1], sys.argv[2]
targets = [int(a, 16) for a in sys.argv[3:]]
d = open(elf, 'rb').read()
SEGS = [(0x10000, 0, 0x6ea078), (0x700000, 0x6f0000, 0x3de30), (0x10000000, 0x730000, 0xb2600), (0x100c0000, 0x7f0000, 0x67600)]
def rd(va):
    for v, o, s in SEGS:
        if v <= va < v + s: return struct.unpack_from('>I', d, va - v + o)[0]
f = sorted((int(x['start'], 16), int(x['end'], 16), int(x['toc'], 16)) for x in json.load(open(fj)))
S = [x[0] for x in f]
def fn(a):
    i = bisect.bisect_right(S, a) - 1
    return f[i] if i >= 0 and f[i][0] <= a < f[i][1] else None
GOT0, GOT1 = 0x727ce0, 0x73de28
for t in targets:
    slots = [g for g in range(GOT0, GOT1, 4) if rd(g) == t]
    print(f'target {t:#x}: got slots {[hex(s) for s in slots]}')
    for a in range(0x10240, 0x661898, 4):
        w, = struct.unpack_from('>I', d, a - 0x10000)
        if (w >> 26) == 32 and ((w >> 16) & 31) == 2:      # lwz rD, d(r2)
            disp = w & 0xffff
            if disp & 0x8000: disp -= 0x10000
            func = fn(a)
            if func and func[2] + disp in slots:
                print(f'   load at {a:#x} in func {func[0]:#x} (toc {func[2]:#x})')

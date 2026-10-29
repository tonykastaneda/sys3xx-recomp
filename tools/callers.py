"""Find direct bl callers of target addresses in GAME_113.elf. usage: callers.py 0xADDR..."""
import struct, sys, json, bisect
d = open('elf/GAME_113.elf', 'rb').read()
f = sorted((int(x['start'], 16), int(x['end'], 16)) for x in json.load(open('out/g113/GAME_113.functions.json')))
S = [x[0] for x in f]
def fn(a):
    i = bisect.bisect_right(S, a) - 1
    return f[i][0] if i >= 0 and f[i][0] <= a < f[i][1] else None
T = {int(a, 16) for a in sys.argv[1:]}
for a in range(0x10200, 0x661898, 4):
    w, = struct.unpack_from('>I', d, a - 0x10000)
    if (w >> 26) == 18 and (w & 1):
        off = w & 0x3fffffc
        if off & 0x2000000: off -= 0x4000000
        if a + off in T: print(f'{a+off:#x} called from {a:#x} in fn {fn(a):#x}' if fn(a) else f'{a+off:#x} called from {a:#x}')

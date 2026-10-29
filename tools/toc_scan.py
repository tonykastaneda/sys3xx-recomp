"""Find direct bl calls (bl+nop = same-TOC by ABI) whose caller/target the loader put in different TOC groups."""
import struct, json, bisect, sys
elf, fj = sys.argv[1], sys.argv[2]
d = open(elf, 'rb').read()
f = sorted(((int(x['start'],16), int(x['end'],16), int(x['toc'],16)) for x in json.load(open(fj))))
starts = [x[0] for x in f]
def fn(a):
    i = bisect.bisect_right(starts, a) - 1
    return f[i] if i >= 0 and f[i][0] <= a < f[i][1] else None
TEXT0, TEXT1 = 0x10240, 0x661898
bad = {}
for a in range(TEXT0, TEXT1, 4):
    w, = struct.unpack_from('>I', d, a - 0x10000)
    if (w >> 26) == 18 and (w & 3) == 1:            # bl
        off = w & 0x3fffffc
        if off & 0x2000000: off -= 0x4000000
        t = a + off
        nxt, = struct.unpack_from('>I', d, a + 4 - 0x10000)
        c, tg = fn(a), fn(t)
        if c and tg and c[2] != tg[2] and nxt == 0x60000000:
            bad.setdefault((tg[0], tg[2]), []).append((a, c[0], c[2]))
print(len(bad), 'targets called bl+nop across TOC groups')
for (t, tt), cs in sorted(bad.items())[:40]:
    print(f'target {t:#x} toc {tt:#x}  callers {len(cs)} e.g. {cs[0][0]:#x} in fn {cs[0][1]:#x} toc {cs[0][2]:#x}')

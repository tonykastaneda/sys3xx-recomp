"""Disassemble GAME_113.elf range with import names and string hints. usage: dis.py START END"""
import struct, sys, json
from capstone import *
d = open('elf/GAME_113.elf', 'rb').read()
SEGS = [(0x10000, 0, 0x6ea078), (0x700000, 0x6f0000, 0x3de30), (0x10000000, 0x730000, 0xb2600), (0x100c0000, 0x7f0000, 0x67600)]
def rd(va):
    for v, o, s in SEGS:
        if v <= va < v + s: return struct.unpack_from('>I', d, va - v + o)[0]
def cstr(va):
    for v, o, s in SEGS:
        if v <= va < v + s:
            off = va - v + o; e = d.find(b'\0', off, off + 80)
            t = d[off:e]
            return t.decode('latin1') if t and all(32 <= c < 127 or c in (9, 10) for c in t) else None
imp = {int(x['stub'], 16): x for x in json.load(open('out/g113/GAME_113.imports.json'))}
funcs = {int(x['start'], 16): int(x['toc'], 16) for x in json.load(open('out/g113/GAME_113.functions.json'))}
a0, a1 = int(sys.argv[1], 16), int(sys.argv[2], 16)
toc = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x72fce0
md = Cs(CS_ARCH_PPC, CS_MODE_64 | CS_MODE_BIG_ENDIAN)
for i in md.disasm(d[a0 - 0x10000:a1 - 0x10000], a0):
    note = ''
    if i.mnemonic == 'bl':
        t = int(i.op_str, 16)
        note = f"  <{imp[t]['library']} {imp[t]['nid']}>" if t in imp else ''
    if i.mnemonic in ('lwz', 'ld') and i.op_str.endswith('(r2)'):
        disp = int(i.op_str.split(',')[1].strip().split('(')[0], 16)
        v = rd(toc + disp)
        s = cstr(v) if v else None
        note = f'  ; [{toc+disp:#x}] = {v:#x}' + (f' "{s}"' if s else '') if v is not None else ''
    print(f'{i.address:#x}  {i.mnemonic:8} {i.op_str}{note}')

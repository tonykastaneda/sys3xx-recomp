"""Symbolize RVAs against an lld-link /MAP file. usage: sym.py MAP rva [rva...]  (or pipe a crash log on stdin)"""
import re, sys, bisect
rows = []
for line in open(sys.argv[1], errors='ignore'):
    m = re.match(r'\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+(.*)', line)
    if m: rows.append((int(m.group(2), 16), m.group(1), m.group(3).strip()))
rows.sort(); A = [r[0] for r in rows]
rvas = sys.argv[2:] or re.findall(r'rva=0x([0-9A-Fa-f]+)', sys.stdin.read())
for r in rvas:
    v = 0x140000000 + int(r, 16)
    i = bisect.bisect_right(A, v) - 1
    print(f'{int(r,16):#x}  {rows[i][1]}+{v-rows[i][0]:#x}  [{rows[i][2]}]' if i >= 0 else r)

"""Convert a debug (fake-signed, unencrypted) PS3 SELF to a plain ELF.

Arcade System 357 executables in this dump are debug SELFs (key revision 0x8000):
segments are stored in clear, optionally zlib-compressed. This rebuilds the ELF
from the SELF's extended header, program headers and section info table.

Usage: python dself2elf.py IN.SELF OUT.elf
"""
import struct
import sys
import zlib


def be(fmt, data, off):
    return struct.unpack_from(">" + fmt, data, off)


def convert(src: bytes) -> bytes:
    if src[:4] != b"SCE\0":
        raise ValueError("not a SELF")
    key_rev, = be("H", src, 8)
    if key_rev != 0x8000:
        raise ValueError(f"not a debug SELF (key revision {key_rev:#x}); needs real decryption")
    # Debug SELFs carry the original ELF file verbatim right after the header.
    header_len, data_len = be("QQ", src, 0x10)
    if src[header_len:header_len + 4] == b"\x7fELF":
        return src[header_len:header_len + data_len]

    (_ext_type, _appinfo, elf_off, phdr_off, shdr_off, sect_info_off,
     _sv_off, _ci_off, _ci_size) = be("9Q", src, 0x20)

    ehdr = src[elf_off:elf_off + 64]
    if ehdr[:4] != b"\x7fELF" or ehdr[4] != 2:
        raise ValueError("embedded ELF header is not ELF64")
    e_phoff, e_shoff = be("QQ", ehdr, 0x20)
    e_phentsize, e_phnum, e_shentsize, e_shnum = be("HHHH", ehdr, 0x36)

    phdrs = [src[phdr_off + i * e_phentsize: phdr_off + (i + 1) * e_phentsize] for i in range(e_phnum)]
    size = max(e_phoff + e_phnum * e_phentsize, 64)
    segs = []
    for i, ph in enumerate(phdrs):
        _type, _flags, p_offset, _va, _pa, p_filesz = be("IIQQQQ", ph, 0)
        s_off, s_size, compressed, _u1, _u2, _encrypted = be("QQIIII", src, sect_info_off + i * 32)
        data = src[s_off:s_off + s_size]
        if compressed == 2 and p_filesz:
            data = zlib.decompress(data)
        segs.append((p_offset, data[:p_filesz] if p_filesz else b""))
        size = max(size, p_offset + p_filesz)

    out = bytearray(size)
    out[0:64] = ehdr
    for i, ph in enumerate(phdrs):
        out[e_phoff + i * e_phentsize: e_phoff + (i + 1) * e_phentsize] = ph
    for off, data in segs:
        out[off:off + len(data)] = data

    # Section headers are optional; keep them when present so tools see symbols/sections.
    if e_shnum and shdr_off:
        sh = src[shdr_off:shdr_off + e_shnum * e_shentsize]
        if len(out) < e_shoff + len(sh):
            out.extend(b"\0" * (e_shoff + len(sh) - len(out)))
        out[e_shoff:e_shoff + len(sh)] = sh
    return bytes(out)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as f:
        data = convert(f.read())
    with open(sys.argv[2], "wb") as f:
        f.write(data)
    print(f"{sys.argv[2]}: {len(data)} bytes")

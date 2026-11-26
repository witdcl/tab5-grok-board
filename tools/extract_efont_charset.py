#!/usr/bin/env python3
"""从已编译的 lgfx_efont_cn_*.o 提取 Tab5 efont 可绘字符 BMP 位图。

用法（在工程根目录）:
  python3 tools/extract_efont_charset.py

输出:
  tools/efont_cn_allowed.bin
  tools/efont_cn_allowed.meta.json
"""

from __future__ import annotations

import json
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OBJ = next(
    ROOT.glob(
        ".pio/build/tab5_weread/**/lgfx_efont_cn.c.o"
    ),
    None,
)
OUT_BIN = Path(__file__).resolve().parent / "efont_cn_allowed.bin"
OUT_META = Path(__file__).resolve().parent / "efont_cn_allowed.meta.json"
NM = Path.home() / ".platformio/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-nm"


def extract_font_blob(obj: Path, symbol: str = "lgfx_efont_cn_24") -> bytes:
    data = obj.read_bytes()
    assert data[:4] == b"\x7fELF"
    e_shoff = struct.unpack_from("<I", data, 32)[0]
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 46)

    def sh(i: int):
        off = e_shoff + i * e_shentsize
        return struct.unpack_from("<IIIIIIIIII", data, off)

    shstr = sh(e_shstrndx)
    strtab = data[shstr[4] : shstr[4] + shstr[5]]
    sections = []
    for i in range(e_shnum):
        name_i, _typ, _flags, addr, off, size = sh(i)[:6]
        n = strtab[name_i : strtab.find(b"\0", name_i)].decode()
        sections.append((n, addr, off, size))

    symtab = strtab_sym = None
    for n, _a, o, sz in sections:
        if n == ".symtab":
            symtab = (o, sz)
        if n == ".strtab":
            strtab_sym = (o, sz)
    assert symtab and strtab_sym
    st_data = data[symtab[0] : symtab[0] + symtab[1]]
    str_data = data[strtab_sym[0] : strtab_sym[0] + strtab_sym[1]]
    for i in range(0, len(st_data), 16):
        st_name, st_value, st_size, _info, _other, st_shndx = struct.unpack_from("<IIIBBH", st_data, i)
        name = str_data[st_name : str_data.find(b"\0", st_name)].decode()
        if name == symbol:
            _sec_name, sec_addr, sec_off, _sec_sz = sections[st_shndx]
            file_off = sec_off + (st_value - sec_addr)
            return data[file_off : file_off + st_size]
    raise RuntimeError(f"symbol {symbol} not found in {obj}")


def get_glyph(font: bytes, encoding: int) -> int | None:
    def be16(i: int) -> int:
        return (font[i] << 8) | font[i + 1]

    base = 23
    if encoding <= 255:
        if encoding >= ord("a"):
            p = base + be16(19)
        elif encoding >= ord("A"):
            p = base + be16(17)
        else:
            p = base
        while p + 1 < len(font) and font[p + 1]:
            if font[p] == encoding:
                return p + 2
            p += font[p + 1]
        return None
    p = base + be16(21)
    lut = p
    while True:
        if lut + 4 > len(font):
            return None
        p += (font[lut] << 8) | font[lut + 1]
        e = (font[lut + 2] << 8) | font[lut + 3]
        lut += 4
        if e >= encoding:
            break
    while p + 2 < len(font):
        e = (font[p] << 8) | font[p + 1]
        if e == 0:
            return None
        if e == encoding:
            return p + 3
        step = font[p + 2]
        if step == 0:
            return None
        p += step
    return None


def main() -> None:
    obj = OBJ
    if obj is None:
        # fallback glob
        hits = list(ROOT.glob(".pio/build/**/lgfx_efont_cn.c.o"))
        if not hits:
            raise SystemExit("找不到 lgfx_efont_cn.c.o，请先编译 tab5_weread")
        obj = hits[0]
    font = extract_font_blob(obj)
    cps = {c for c in range(0x20, 0x10000) if get_glyph(font, c) is not None}
    bitset = bytearray((0x10000 + 7) // 8)
    for c in cps:
        bitset[c >> 3] |= 1 << (c & 7)
    OUT_BIN.write_bytes(bitset)
    OUT_META.write_text(
        json.dumps(
            {"font": "lgfx_efont_cn_24", "count": len(cps), "has_yue": 0x73A5 in cps},
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(f"wrote {OUT_BIN} glyphs={len(cps)} has_yue={0x73A5 in cps}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# Шрифт для тестов без монитора: глифы 040..0137 из ПЗУ монитора (0112036 = код 020,
# по 10 байт) и таблица удвоения битов (байт глифа -> слово экрана в 32 колонки).
#   python3 mkfont.py ../../roms/monit10.rom font.s
import sys
rom = open(sys.argv[1], 'rb').read()
base = 0o112036 - 0o100000
out = ["/ Сгенерировано mkfont.py из monit10.rom: глифы 040..0137, удвоение битов.", "font:"]
for c in range(0o40, 0o140):
    g = rom[base + (c - 0o20) * 10: base + (c - 0o20) * 10 + 10]
    out.append("\t.byte\t" + ", ".join("0%o" % b for b in g) + "\t/ %03o" % c)
out.append("\t.even")
out.append("dbl:")
for i in range(0, 256, 8):
    ws = []
    for b in range(i, i + 8):
        w = 0
        for k in range(8):
            if b >> k & 1: w |= 3 << (2 * k)
        ws.append("0%o" % w)
    out.append("\t.word\t" + ", ".join(ws))
open(sys.argv[2], 'w').write("\n".join(out) + "\n")

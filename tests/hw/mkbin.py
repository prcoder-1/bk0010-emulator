#!/usr/bin/env python3
# Плоский образ с адреса 01000 -> .BIN БК (адрес загрузки, длина, данные).
import struct, sys
data = open(sys.argv[1], 'rb').read()
if len(data) & 1: data += b'\0'
open(sys.argv[2], 'wb').write(struct.pack('<HH', 0o1000, len(data)) + data)

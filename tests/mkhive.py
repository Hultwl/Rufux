#!/usr/bin/env python3
"""Write a minimal but valid Windows registry hive (root key with one
subkey, "Setup") so tests can exercise offline registry edits without a
real Windows image. Usage: mkhive.py OUT"""
import struct, sys

def cell(payload):
    n = 4 + len(payload)
    n = (n + 7) & ~7
    return struct.pack('<i', -n) + payload + b'\0' * (n - 4 - len(payload))

def nk(name, flags, parent, nsub, sublist, sk, root=False):
    nb = name.encode('ascii')
    return (b'nk' + struct.pack('<H', flags) + struct.pack('<Q', 0) + struct.pack('<I', 0) +
            struct.pack('<I', parent) + struct.pack('<I', nsub) + struct.pack('<I', 0) +
            struct.pack('<I', sublist) + struct.pack('<I', 0xFFFFFFFF) +
            struct.pack('<I', 0) + struct.pack('<I', 0xFFFFFFFF) +      # values
            struct.pack('<I', sk) + struct.pack('<I', 0xFFFFFFFF) +     # sk, class
            struct.pack('<I', len(nb) * 2 if nsub else 0) + struct.pack('<I', 0) +
            struct.pack('<I', 0) + struct.pack('<I', 0) + struct.pack('<I', 0) +
            struct.pack('<H', len(nb)) + struct.pack('<H', 0) + nb)

BASE = 0x20                      # first cell offset inside the hbin
sd = bytes([1, 0, 4, 0x80]) + bytes(16)   # placeholder security descriptor
# lay out cells: root nk, sk, lf list, Setup nk
def build():
    # sizes are fixed, so compute offsets first with placeholders
    dummy = nk('ROOT', 0x2C, 0xFFFFFFFF, 1, 0, 0)
    root_sz = len(cell(dummy))
    sk_payload = b'sk' + b'\0\0' + struct.pack('<III', 0, 0, 1) + struct.pack('<I', len(sd)) + sd
    sk_sz = len(cell(sk_payload))
    lf_payload = b'lf' + struct.pack('<H', 1) + struct.pack('<II', 0, 0)
    lf_sz = len(cell(lf_payload))
    root_off = BASE
    sk_off = root_off + root_sz
    lf_off = sk_off + sk_sz
    setup_off = lf_off + lf_sz
    sk_payload = b'sk' + b'\0\0' + struct.pack('<III', sk_off, sk_off, 1) + struct.pack('<I', len(sd)) + sd
    lf_payload = b'lf' + struct.pack('<H', 1) + struct.pack('<I', setup_off) + b'Setu'
    cells = (cell(nk('ROOT', 0x2C, 0xFFFFFFFF, 1, lf_off, sk_off)) + cell(sk_payload) +
             cell(lf_payload) + cell(nk('Setup', 0x20, root_off, 0, 0xFFFFFFFF, sk_off)))
    return root_off, cells

root_off, cells = build()
free = 0x1000 - BASE - len(cells)
body = cells + struct.pack('<i', free) + b'\0' * (free - 4)
hbin = b'hbin' + struct.pack('<II', 0, 0x1000) + b'\0' * 8 + struct.pack('<Q', 0) + struct.pack('<I', 0) + body
assert len(hbin) == 0x1000, len(hbin)
hdr = bytearray(4096)
hdr[0:4] = b'regf'
struct.pack_into('<II', hdr, 4, 1, 1)
struct.pack_into('<IIII', hdr, 0x14, 1, 3, 0, 1)
# root cell offset is relative to the first hbin (which starts at 0x1000)
struct.pack_into('<III', hdr, 0x24, root_off, 0x1000, 1)
name = 'SYSTEM'.encode('utf-16-le')
hdr[0x30:0x30 + len(name)] = name
x = 0
for i in range(0, 0x1FC, 4):
    x ^= struct.unpack_from('<I', hdr, i)[0]
struct.pack_into('<I', hdr, 0x1FC, x)
open(sys.argv[1], 'wb').write(bytes(hdr) + hbin)

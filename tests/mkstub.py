#!/usr/bin/env python3
"""Writes a tiny 16-bit "bootmgr" for BIOS boot tests: when the loader chain reaches it,
it prints BOOTMGR-OK on COM1 and halts. Usage: mkstub.py OUT"""
import struct, sys
msg = b"BOOTMGR-OK\r\n\0"
code = bytearray(b"\xfc\x0e\x1f\xbe\x00\x00")            # cld; push cs; pop ds; mov si,msg
code += b"\xac\x84\xc0\x74\x06\xba\xf8\x03\xee\xeb\xf5"   # lodsb; test al,al; jz done; mov dx,3f8; out dx,al; jmp
code += b"\xf4\xeb\xfd"                                   # done: hlt; jmp done
code[4:6] = struct.pack("<H", len(code))
code += msg
open(sys.argv[1], "wb").write(bytes(code).ljust(4096, b"\0"))

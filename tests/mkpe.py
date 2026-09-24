#!/usr/bin/env python3
"""Test helper for test_bootcheck.sh: builds a minimal PE32+ EFI image, computes its
Authenticode SHA-256 independently of Rufux, and writes DBX files.
  mkpe.py pe OUT [SBAT_TEXT]      write an unsigned EFI application (optionally with a .sbat section)
  mkpe.py hash FILE               print the Authenticode SHA-256 (works on signed files too)
  mkpe.py tbs CERT.der            print the SHA-256 of the certificate's TBSCertificate
  mkpe.py dbx OUT [--sha HEX]... [--tbs HEX]...   write an efiauth2-style DBX"""
import sys, struct, hashlib

def pe(sbat=None):
    text = b'\xc3' + b'\x90' * 0x1ff
    secs = [(b'.text', text, 0x60000020)]
    if sbat is not None:
        secs.append((b'.sbat', sbat.encode() + b'\0', 0x40000040))
    hdr_size = 0x400
    opt_size = 112 + 16 * 8
    raw = []; off = hdr_size
    for n, d, c in secs:
        rs = (len(d) + 0x1ff) & ~0x1ff
        raw.append((n, d, c, off, rs)); off += rs
    dos = b'MZ' + b'\0' * 0x3a + struct.pack('<I', 0x80)
    dos = dos.ljust(0x80, b'\0')
    coff = b'PE\0\0' + struct.pack('<HHIIIHH', 0x8664, len(secs), 0, 0, 0, opt_size, 0x0022)
    opt = struct.pack('<HBBIIIIIQIIHHHHHHIIIIHHQQQQII', 0x20b, 2, 0, 0x200, 0x200, 0, 0x1000, 0x1000,
                      0x140000000, 0x1000, 0x200, 0, 0, 0, 0, 6, 0, 0,
                      0x1000 * (len(secs) + 1), hdr_size, 0, 10, 0, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    opt += b'\0' * (16 * 8)
    sh = b''
    va = 0x1000
    for n, d, c, o, rs in raw:
        sh += n.ljust(8, b'\0') + struct.pack('<IIIIIIHHI', len(d), va, rs, o, 0, 0, 0, 0, c)
        va += 0x1000
    head = (dos + coff + opt + sh).ljust(hdr_size, b'\0')
    body = b''.join(d.ljust(rs, b'\0') for n, d, c, o, rs in raw)
    return head + body

def authenticode(d, alg='sha256'):
    p = struct.unpack_from('<I', d, 0x3c)[0]
    opt = p + 24; plus = struct.unpack_from('<H', d, opt)[0] == 0x20b
    chk = opt + 64; dd = opt + (112 if plus else 96)
    cert_off, cert_size = struct.unpack_from('<II', d, dd + 32)
    soh = struct.unpack_from('<I', d, opt + 60)[0]
    nsec = struct.unpack_from('<H', d, p + 6)[0]; osz = struct.unpack_from('<H', d, p + 20)[0]
    h = hashlib.new(alg)
    h.update(d[:chk]); h.update(d[chk + 4:dd + 32]); h.update(d[dd + 40:soh])
    secs = []
    for i in range(nsec):
        o = opt + osz + 40 * i
        rs, rp = struct.unpack_from('<II', d, o + 16)
        secs.append((rp, rs))
    total = soh
    for rp, rs in sorted(secs):
        if rs: h.update(d[rp:rp + rs]); total += rs
    if len(d) > total:
        h.update(d[total:len(d) - cert_size if cert_size else len(d)])
    return h.hexdigest()

def der_first_child_of_first_seq(der):
    def hdr(b, i):
        l = b[i + 1]
        if l < 0x80: return 2, l
        n = l & 0x7f; return 2 + n, int.from_bytes(b[i + 2:i + 2 + n], 'big')
    h, l = hdr(der, 0)          # Certificate ::= SEQUENCE
    h2, l2 = hdr(der, h)        # TBSCertificate ::= SEQUENCE
    return der[h:h + h2 + l2]

def dbx(out, shas, tbss):
    def lst(guid, entries, extra):
        body = b''.join(bytes(16) + bytes.fromhex(e) + extra for e in entries)
        ssz = 16 + 32 + len(extra)
        return guid + struct.pack('<III', 28 + len(body), 0, ssz) + body
    G_SHA = bytes.fromhex('2616c4c14c509240aca941f936934328')
    G_X509 = bytes.fromhex('92a4d23bc09679 40b420fcf98ef103ed'.replace(' ', ''))
    data = b''
    if shas: data += lst(G_SHA, shas, b'')
    if tbss: data += lst(G_X509, tbss, bytes(16))
    auth = bytes(16) + struct.pack('<IHH', 24, 0x200, 0xEF1) + bytes(16)  # EFI_TIME + WIN_CERTIFICATE_UEFI_GUID (no payload)
    open(out, 'wb').write(auth + data)

a = sys.argv[1:]
if a[0] == 'pe': open(a[1], 'wb').write(pe(a[2] if len(a) > 2 else None))
elif a[0] == 'hash': print(authenticode(open(a[1], 'rb').read()))
elif a[0] == 'tbs': print(hashlib.sha256(der_first_child_of_first_seq(open(a[1], 'rb').read())).hexdigest())
elif a[0] == 'dbx':
    shas = [a[i + 1] for i in range(2, len(a) - 1) if a[i] == '--sha']
    tbss = [a[i + 1] for i in range(2, len(a) - 1) if a[i] == '--tbs']
    dbx(a[1], shas, tbss)

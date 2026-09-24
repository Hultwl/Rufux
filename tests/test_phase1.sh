#!/usr/bin/env bash
# Phase 1 acceptance tests. Usage: test_phase1.sh <path-to-rufux-binary>
set -u
RUFUX="${1:-./build/rufux}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
pass=0; fail=0
ok() { echo "PASS: $1"; pass=$((pass+1)); }
bad() { echo "FAIL: $1"; fail=$((fail+1)); }

[ -x "$RUFUX" ] || { echo "binary not found: $RUFUX"; exit 1; }

# 1. list (human + json)
if "$RUFUX" list >/dev/null 2>&1; then ok "list"; else bad "list"; fi
if "$RUFUX" list --json | grep -q '^\['; then ok "list --json"; else bad "list --json"; fi

# 2. make fake ISO: PVD at 16*2048 + boot record at 17*2048
FAKE="$TMP/fake.iso"
python3 - "$FAKE" <<'EOF'
import sys
p = sys.argv[1]
size = 20*2048
b = bytearray(size)
# PVD
b[16*2048+0] = 1
b[16*2048+1:16*2048+6] = b'CD001'
b[16*2048+40:16*2048+40+11] = b'RUFUX_TEST '
# boot record
b[17*2048+0] = 0
b[17*2048+1:17*2048+6] = b'CD001'
b[17*2048+7:17*2048+30] = b'EL TORITO SPECIFICATION'
open(p,'wb').write(b)
EOF
if "$RUFUX" probe "$FAKE" | grep -q RUFUX_TEST; then ok "probe label"; else bad "probe label"; fi
if "$RUFUX" probe "$FAKE" --detail | grep -q 'bootable: yes'; then ok "probe bootable"; else bad "probe bootable"; fi
if "$RUFUX" probe "$TMP/missing.iso" >/dev/null 2>&1; then bad "probe missing should fail"; else ok "probe missing fails"; fi

# 2b. EFI platform id from the El Torito catalog validation entry
# (structural parse; a stray 0xEF byte elsewhere must not trigger it)
python3 - "$TMP/efi.iso" "$TMP/noefi.iso" <<'EOF'
import sys, struct
for p, plat in ((sys.argv[1], 0xEF), (sys.argv[2], 0x00)):
    size = 24*2048
    b = bytearray(size)
    b[16*2048+0] = 1
    b[16*2048+1:16*2048+6] = b'CD001'
    b[16*2048+40:16*2048+44] = b'EFI1'
    b[16*2048+100] = 0xEF  # poison: must not count as EFI
    b[17*2048+0] = 0
    b[17*2048+1:17*2048+6] = b'CD001'
    b[17*2048+7:17*2048+30] = b'EL TORITO SPECIFICATION'
    struct.pack_into('<I', b, 17*2048+0x47, 20)  # catalog at sector 20
    b[20*2048+0] = 0x01  # validation entry header
    b[20*2048+1] = plat  # platform id
    open(p, 'wb').write(b)
EOF
if "$RUFUX" probe "$TMP/efi.iso" --detail | grep -q 'efi_hint: yes'; then ok "probe efi platform"; else bad "probe efi platform"; fi
if "$RUFUX" probe "$TMP/noefi.iso" --detail | grep -q 'efi_hint: no'; then ok "probe non-efi ignored"; else bad "probe non-efi ignored"; fi

# 3. checksum matches sha256sum
if [ "$(sha256sum "$FAKE" | cut -d' ' -f1)" = "$("$RUFUX" checksum "$FAKE" | cut -d' ' -f1)" ]; then
  ok "checksum matches sha256sum"
else
  bad "checksum matches sha256sum"
fi
# 3b. all four algos match system tools
for a in "md5:md5sum" "sha1:sha1sum" "sha512:sha512sum"; do
  algo="${a%%:*}"; tool="${a##*:}"
  if [ "$($tool "$FAKE" | cut -d' ' -f1)" = "$("$RUFUX" checksum "$FAKE" --algo "$algo" | cut -d' ' -f1)" ]; then
    ok "checksum $algo"
  else
    bad "checksum $algo"
  fi
done

# 4. dry-run to a file target (needs --allow-file to pass target check, writes nothing)
DST="$TMP/dst.img"
truncate -s 10M "$DST"
BEFORE="$(sha256sum "$DST" | cut -d' ' -f1)"
if "$RUFUX" write "$FAKE" "$DST" --dry-run --allow-file >/dev/null 2>&1; then ok "write --dry-run"; else bad "write --dry-run"; fi
AFTER="$(sha256sum "$DST" | cut -d' ' -f1)"
[ "$BEFORE" = "$AFTER" ] && ok "dry-run writes nothing" || bad "dry-run writes nothing"

# 5. real write to file + verify
if "$RUFUX" write "$FAKE" "$DST" --real --allow-file --yes --verify >/dev/null 2>&1; then ok "write --real --verify"; else bad "write --real --verify"; fi
# dst head must equal src
if cmp -n "$(stat -c%s "$FAKE")" "$FAKE" "$DST"; then ok "written bytes match"; else bad "written bytes match"; fi

# 6. safety: real write without --yes must fail
truncate -s 10M "$DST"
if "$RUFUX" write "$FAKE" "$DST" --real --allow-file >/dev/null 2>&1; then bad "write without --yes should fail"; else ok "write without --yes refused"; fi

# 7. safety: file target without --allow-file must fail
if "$RUFUX" write "$FAKE" "$DST" --dry-run >/dev/null 2>&1; then bad "file target without --allow-file should fail"; else ok "file target refused by default"; fi

# 8. safety: source and target must differ
if "$RUFUX" write "$FAKE" "$FAKE" --dry-run --allow-file >/dev/null 2>&1; then bad "src==dst should fail"; else ok "src==dst refused"; fi

# 9. download-windows lists what it can fetch without touching the network, and rejects bad input
# (the real download is covered by test_msdl.sh against a local stand-in server)
if "$RUFUX" download-windows --list 2>&1 | grep -q "Windows 11"; then ok "download-windows --list"; else bad "download-windows --list"; fi
if "$RUFUX" download-windows --version 12 >/dev/null 2>&1; then bad "download-windows accepted version 12"; else ok "download-windows rejects an unknown version"; fi

echo "--- $pass passed, $fail failed ---"
[ "$fail" -eq 0 ]

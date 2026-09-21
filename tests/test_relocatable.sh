#!/usr/bin/env bash
# Relocatable bundle: resources next to the binary (<exedir>/../share/...)
# must win over host paths, the way an AppImage lays them out.
set -u
RUFUX="${1:?usage: test_relocatable.sh <rufux-binary>}"
pass=0; fail=0
ok() { echo "PASS: $1"; pass=$((pass+1)); }
bad() { echo "FAIL: $1"; fail=$((fail+1)); }

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/usr/bin" "$T/usr/share/syslinux"
cp "$RUFUX" "$T/usr/bin/rufux"
# Content is irrelevant for --dry-run (only readability is probed).
head -c 440 /dev/zero > "$T/usr/share/syslinux/mbr.bin"
head -c 440 /dev/zero > "$T/usr/share/syslinux/gptmbr.bin"
touch "$T/disk.img"

for kind in bios gpt; do
  out="$("$T/usr/bin/rufux" install-boot "$T/disk.img" --mbr "$kind" --dry-run --allow-file 2>&1)"
  # The printed path keeps the un-normalized .. (usr/bin/../share), so
  # match the bundle root and the basename separately.
  if printf '%s' "$out" | grep -qF "$T" && printf '%s' "$out" | grep -q "share/syslinux/$([ "$kind" = gpt ] && echo gptmbr.bin || echo mbr.bin)"; then
    ok "bundled $kind MBR preferred"
  else
    bad "bundled $kind MBR ignored: $out"
  fi
done

# Without a bundle next to it, the host copy must still resolve.
out="$(cp "$RUFUX" "$T/plain-rufux" && "$T/plain-rufux" install-boot "$T/disk.img" --mbr bios --dry-run --allow-file 2>&1)"
if printf '%s' "$out" | grep -q "syslinux.*mbr.bin"; then
  ok "host MBR fallback works"
else
  bad "host MBR fallback broken: $out"
fi

echo "--- $pass passed, $fail failed ---"
[ "$fail" -eq 0 ]

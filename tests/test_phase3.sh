#!/usr/bin/env bash
# Phase 3 acceptance: mount planning, Secure Boot, EFI validation,
# packaging files, translations, update-check handling.
set -u
RUFUX="${1:-./build/rufux}"
SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
pass=0; fail=0
ok() { echo "PASS: $1"; pass=$((pass+1)); }
bad() { echo "FAIL: $1"; fail=$((fail+1)); }
[ -x "$RUFUX" ] || { echo "binary not found: $RUFUX"; exit 1; }

# 1. mount/umount dry-run (no udisks2 touch)
"$RUFUX" mount /dev/sdz1 --dry-run 2>&1 | grep -q "udisksctl mount" && ok "mount --dry-run" || bad "mount --dry-run"
"$RUFUX" umount /dev/sdz1 --dry-run 2>&1 | grep -q "udisksctl unmount" && ok "umount --dry-run" || bad "umount --dry-run"

# 2. secureboot-status parses
if "$RUFUX" secureboot-status | grep -Eq "secure-boot: (enabled|disabled|unknown)"; then
  ok "secureboot-status ($("$RUFUX" secureboot-status))"
else
  bad "secureboot-status"
fi

# 3. validate-efi: bundled Rufus/UEFI bootloaders are real EFI PEs
EFI="$SRC_DIR/res/md5/bootx64.efi"
if [ -f "$EFI" ]; then
  if "$RUFUX" validate-efi "$EFI" | grep -q "subsystem 10"; then ok "validate-efi bootx64.efi"; else bad "validate-efi bootx64.efi"; fi
  for f in bootaa64.efi bootia32.efi bootarm.efi; do
    "$RUFUX" validate-efi "$SRC_DIR/res/md5/$f" >/dev/null 2>&1 && ok "validate-efi $f" || bad "validate-efi $f"
  done
else
  bad "efi fixture missing"
fi
# non-EFI must fail
if "$RUFUX" validate-efi /bin/ls >/dev/null 2>&1; then bad "validate-efi rejects non-EFI"; else ok "validate-efi rejects non-EFI"; fi
if "$RUFUX" validate-efi "$TMP/missing.efi" >/dev/null 2>&1; then bad "validate-efi rejects missing"; else ok "validate-efi rejects missing"; fi

# 4. privilege guard: real block op without root must refuse clearly
if [ "$(id -u)" -ne 0 ]; then
  BLK=""
  for d in /dev/sda /dev/nvme0n1 /dev/mmcblk0 /dev/vda; do
    [ -b "$d" ] && { BLK="$d"; break; }
  done
  if [ -z "$BLK" ]; then
    ok "privilege guard message (skipped: no block device present)"
  elif "$RUFUX" write "$EFI" "$BLK" --real --yes 2>&1 | grep -qi "root\|sudo\|pkexec"; then
    ok "privilege guard message"
  else
    bad "privilege guard message"
  fi
else
  ok "privilege guard message (skipped: running as root)"
fi

# 5. create disk-extract dry-run shows full auto flow incl. udisks2
truncate -s 64M "$TMP/disk.img"
if "$RUFUX" create "$EFI" "$TMP/disk.img" --mode extract --scheme gpt --fs vfat --dry-run --allow-file | grep -q "install-boot"; then
  ok "create disk plan (auto flow)"
else
  bad "create disk plan (auto flow)"
fi

# 6. packaging files exist and parse
[ -f "$SRC_DIR/packaging/PKGBUILD" ] && bash -n "$SRC_DIR/packaging/PKGBUILD" && ok "PKGBUILD syntax" || bad "PKGBUILD syntax"
[ -f "$SRC_DIR/doc/rufux.1" ] && grep -q "secureboot" "$SRC_DIR/doc/rufux.1" && ok "man page" || bad "man page"
python3 -c "import xml.dom.minidom;xml.dom.minidom.parse('$SRC_DIR/res/linux/io.github.hultwl.rufux.policy')" \
  && ok "polkit policy XML" || bad "polkit policy XML"
grep -q "^Exec=rufux --gui$" "$SRC_DIR/res/linux/io.github.hultwl.rufux.desktop" && ok "desktop file" || bad "desktop file"
[ -f "$SRC_DIR/.github/workflows/rufux.yml" ] && ok "CI workflow" || bad "CI workflow"

# 7. translations compile
for lang in fr es; do
  if msgfmt --check -o /dev/null "$SRC_DIR/po/$lang.po" 2>/dev/null; then ok "msgfmt $lang"; else bad "msgfmt $lang"; fi
done

# 8. update-check: offline-tolerant (pass if up-to-date OR clean skip)
UPD_OUT="$("$RUFUX" update-check 2>&1)"
if printf '%s' "$UPD_OUT" | grep -Eq "up to date|latest is"; then
  ok "update-check (online)"
elif printf '%s' "$UPD_OUT" | grep -Eq "network unavailable|offline|GitHub:|no releases"; then
  ok "update-check (skip: $(printf '%s' "$UPD_OUT" | head -c 120))"
else
  bad "update-check"
  echo "--- update-check output ---"; printf '%s\n' "$UPD_OUT"
fi

echo "--- $pass passed, $fail failed ---"
[ "$fail" -eq 0 ]

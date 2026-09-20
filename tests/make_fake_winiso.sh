#!/bin/bash
# Build a small ISO9660+UDF image shaped like Windows install media.
# Contents are placeholders; only names, layout and sizes matter to Rufux.
# Usage: [FAKE_EFI=file.efi] make_fake_winiso.sh OUT.iso [install_wim_mib]
# FAKE_BOOTMGR replaces bootmgr the same way for BIOS boot tests.
# FAKE_EFI replaces efi/boot/bootx64.efi so a firmware boot test can prove the
# UEFI:NTFS chain reached the NTFS partition.
set -e
out=$1; wim_mib=${2:-6}
d=$(mktemp -d)
mkdir -p "$d"/{sources,boot,efi/boot,efi/microsoft/boot,support}
head -c $((wim_mib*1024*1024)) /dev/urandom > "$d/sources/install.wim"
# boot.wim: a real two-image WIM (WinPE + Setup) with a registry hive in each,
# so tests can check the offline registry edit. Falls back to random bytes
# when wimlib-imagex is missing.
if command -v wimlib-imagex >/dev/null && command -v python3 >/dev/null; then
  w=$(mktemp -d); mkdir -p "$w/Windows/System32/config"
  python3 "$(dirname "$0")/mkhive.py" "$w/Windows/System32/config/SYSTEM"
  head -c 65536 /dev/urandom > "$w/Windows/System32/setup.exe"
  wimlib-imagex capture "$w" "$d/sources/boot.wim" "WinPE" --compress=none >/dev/null 2>&1
  wimlib-imagex append  "$w" "$d/sources/boot.wim" "Setup" >/dev/null 2>&1
  rm -rf "$w"
else
  head -c $((2*1024*1024)) /dev/urandom > "$d/sources/boot.wim"
fi
if [ -n "$FAKE_BOOTMGR" ]; then cp "$FAKE_BOOTMGR" "$d/bootmgr"; else head -c 400000 /dev/urandom > "$d/bootmgr"; fi
head -c 65536  /dev/urandom > "$d/bootmgr.efi"
head -c 262144 /dev/urandom > "$d/boot/bcd"
head -c 262144 /dev/urandom > "$d/efi/microsoft/boot/bcd"
if [ -n "$FAKE_EFI" ]; then cp "$FAKE_EFI" "$d/efi/boot/bootx64.efi"; else head -c 1500000 /dev/urandom > "$d/efi/boot/bootx64.efi"; fi
echo "fake setup" > "$d/setup.exe"
echo "fake" > "$d/support/readme.txt"
genisoimage -quiet -iso-level 3 -udf -allow-limited-size -V WIN_FAKE -o "$out" "$d"
rm -rf "$d"

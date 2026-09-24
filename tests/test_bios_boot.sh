#!/bin/bash
# Legacy BIOS boot of Windows installation media on an MBR drive, proven by booting
# it: a stand-in bootmgr prints BOOTMGR-OK on the serial port, so the whole chain
# (MBR -> GRUB in the gap -> find bootmgr -> start it) has to work in QEMU/SeaBIOS.
# Needs root, loop devices, qemu-system-x86_64, the GRUB i386-pc tools and ntfs-3g.
# Usage: test_bios_boot.sh /path/to/rufux      (exit 77 = skipped)
R=$1; HERE=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" = 0 ] || { echo "skip: needs root"; exit 77; }
for t in losetup sfdisk mkfs.ntfs ntfs-3g 7z genisoimage qemu-system-x86_64 grub-mkimage python3; do
  command -v $t >/dev/null || { echo "skip: $t missing"; exit 77; }
done
[ -f /usr/lib/grub/i386-pc/boot.img ] || [ -f /usr/lib/grub2/i386-pc/boot.img ] || { echo "skip: grub i386-pc modules missing"; exit 77; }
T=$(mktemp -d); chmod 755 $T
cleanup() { pkill -f "qemu-system-x86_64.*$T" 2>/dev/null; umount $T/m 2>/dev/null; losetup -D 2>/dev/null; rm -rf $T; }
trap cleanup EXIT
export PATH=$HERE/shims:$PATH RUFUX_SHIM_MNT=$T/shim
fail=0; bad() { echo "FAIL $*"; fail=1; }
python3 $HERE/mkstub.py $T/bootmgr
FAKE_BOOTMGR=$T/bootmgr "$HERE/make_fake_winiso.sh" $T/w.iso 6 || exit 77
mkdir -p $T/m

# boots <image> and waits up to 60 s for BOOTMGR-OK on the serial port
boots() {
  rm -f $T/serial; qemu-system-x86_64 -m 128 -drive file=$1,format=raw -display none \
    -serial file:$T/serial -no-reboot >/dev/null 2>&1 &
  local q=$!
  for i in $(seq 1 60); do grep -q BOOTMGR-OK $T/serial 2>/dev/null && break; sleep 1; done
  kill $q 2>/dev/null; wait $q 2>/dev/null
  grep -q BOOTMGR-OK $T/serial 2>/dev/null
}

FS=ntfs
burn() { # scheme extra-args...
  truncate -s 1G $T/d.img; L=$(losetup -f --show -P $T/d.img)
  "$R" create $T/w.iso $L --mode windows --scheme $1 --fs $FS --label WINTEST --wue nro \
    --real --yes --allow-fixed "${@:2}" >$T/log 2>&1; rc=$?
  tab=$(sfdisk -d $L); losetup -d $L; return $rc
}

# 1. MBR + NTFS: boots on BIOS
burn dos || { bad "dos burn: $(tail -3 $T/log)"; }
grep -q "boots on both UEFI and legacy BIOS" $T/log || bad "dos: log does not say BIOS boot works: $(grep -E 'BIOS|WARNING' $T/log)"
echo "$tab" | grep -q "start= *2048, .*type=7, bootable" || bad "dos: data partition lost its bootable flag or moved: $(echo "$tab" | grep p1)"
[ "$(dd if=$T/d.img bs=1 count=1 2>/dev/null | od -An -tx1 | tr -d ' ')" = eb ] || bad "dos: MBR does not start with boot code"
boots $T/d.img && echo "ok bios-boot-ntfs" || bad "dos: BIOS boot did not reach bootmgr (serial: $(cat $T/serial 2>/dev/null))"
# the scratch files must not be left on the stick
L=$(losetup -f --show -P $T/d.img); partprobe $L; sleep 1; mount -t ntfs-3g ${L}p1 $T/m
[ -e $T/m/.rufux-grub ] && bad "dos: .rufux-grub left on the drive"
[ -f $T/m/bootmgr ] && cmp -s $T/m/bootmgr $T/bootmgr || bad "dos: bootmgr on the drive differs from the ISO's"
umount $T/m; ntfsfix -n ${L}p1 >/dev/null 2>&1 || bad "dos: NTFS reports problems"; losetup -d $L

# 2. GPT: no BIOS boot code, and it says so
burn gpt || bad "gpt burn: $(tail -3 $T/log)"
grep -q "UEFI machines only" $T/log || bad "gpt: log should say UEFI only"
[ "$(dd if=$T/d.img bs=1 count=440 2>/dev/null | tr -d '\0' | wc -c)" = 0 ] || bad "gpt: boot code written to the protective MBR"
echo "ok gpt-untouched"

# 3. GRUB tools unavailable: the burn still succeeds, with a clear warning
RUFUX_GRUB_DIR=/nonexistent burn dos || bad "no-grub burn failed: $(tail -3 $T/log)"
grep -q "WARNING: no legacy BIOS boot code" $T/log || bad "no-grub: no warning"
grep -q "UEFI machines only" $T/log || bad "no-grub: log should say UEFI only"
echo "ok no-grub-fallback"

# 3b. the same with FAT32, where the file system is mounted with vfat (some kernels lack it)
if grep -qw vfat /proc/filesystems || modprobe vfat 2>/dev/null; then
  FS=vfat
  burn dos || bad "fat32 burn: $(tail -3 $T/log)"
  grep -q "boots on both UEFI and legacy BIOS" $T/log || bad "fat32: log does not say BIOS boot works: $(grep -E 'BIOS|WARNING' $T/log)"
  boots $T/d.img && echo "ok bios-boot-fat32" || bad "fat32: BIOS boot did not reach bootmgr (serial: $(cat $T/serial 2>/dev/null))"
  FS=ntfs
else
  echo "note: this kernel cannot mount vfat, FAT32 case not run"
fi

# 4. dry run describes the step and writes nothing
truncate -s 1G $T/d.img; L=$(losetup -f --show -P $T/d.img)
"$R" create $T/w.iso $L --mode windows --scheme dos --fs ntfs --dry-run --allow-fixed >$T/log 2>&1
grep -q "legacy BIOS boot code" $T/log || bad "dry-run does not mention the BIOS step"
losetup -d $L
exit $fail

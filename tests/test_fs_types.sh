#!/bin/bash
# FAT16, ext2 and ext3 really produce those file systems (ext2/ext3 used to come
# out as ext4), the partition type follows the file system, and FAT16 refuses a
# volume it cannot hold. Needs root, loop devices, sfdisk, mkfs.vfat, mkfs.ext4.
# Usage: test_fs_types.sh /path/to/rufux      (exit 77 = skipped)
R=$1; HERE=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" = 0 ] || { echo "skip: needs root"; exit 77; }
for t in losetup sfdisk mkfs.vfat mkfs.ext4 blkid dumpe2fs; do
  command -v $t >/dev/null || { echo "skip: $t missing"; exit 77; }
done
T=$(mktemp -d)
trap 'losetup -D 2>/dev/null; rm -rf $T' EXIT
fail=0; bad() { echo "FAIL $*"; fail=1; }

mkdisk() { truncate -s "$1" $T/d.img; losetup -f --show -P $T/d.img; }

# FAT16, dos scheme: partition type 0e, file system FAT16
L=$(mkdisk 512M)
"$R" partition $L --scheme dos --layout single --fs fat16 --real --yes --allow-fixed >$T/log 2>&1 || bad "fat16 partition: $(tail -2 $T/log)"
sfdisk -d $L | grep -q "type=e" || bad "fat16: MBR partition type is not 0e"
"$R" format ${L}p1 --fs fat16 --label F16TEST --real --yes --allow-fixed >$T/log 2>&1 || bad "fat16 format: $(tail -2 $T/log)"
[ "$(blkid -o value -s TYPE ${L}p1)" = vfat ] || bad "fat16: not a vfat volume"
# FAT type is in the BPB: FAT16 has the string "FAT16" at offset 54
[ "$(dd if=${L}p1 bs=1 skip=54 count=5 2>/dev/null)" = FAT16 ] || bad "fat16: boot sector does not say FAT16"
losetup -d $L; echo "ok fat16"

# FAT16 on a volume over 4 GiB must be refused, not silently mangled
truncate -s 6G $T/big.img
L=$(losetup -f --show -P $T/big.img)
"$R" partition $L --scheme dos --layout single --fs vfat --real --yes --allow-fixed >/dev/null 2>&1
if "$R" format ${L}p1 --fs fat16 --real --yes --allow-fixed >$T/log 2>&1; then bad "fat16 accepted a 6 GiB volume"
else grep -q "limited to 4 GiB" $T/log || bad "fat16 over 4 GiB: wrong message: $(tail -1 $T/log)"; fi
losetup -d $L; rm -f $T/big.img; echo "ok fat16-limit"

# A 3 GiB FAT16 volume needs 64 KiB clusters and must work
truncate -s 3G $T/mid.img
L=$(losetup -f --show -P $T/mid.img)
"$R" partition $L --scheme dos --layout single --fs fat16 --real --yes --allow-fixed >/dev/null 2>&1
"$R" format ${L}p1 --fs fat16 --real --yes --allow-fixed >$T/log 2>&1 || bad "fat16 3 GiB: $(tail -2 $T/log)"
[ "$(blkid -o value -s TYPE ${L}p1)" = vfat ] || bad "fat16 3 GiB: not vfat"
losetup -d $L; rm -f $T/mid.img; echo "ok fat16-3g"

# ext2 has no journal, ext3 has one but no extents, ext4 has both
for fs in ext2 ext3 ext4; do
  L=$(mkdisk 256M)
  "$R" partition $L --scheme gpt --layout single --fs $fs --real --yes --allow-fixed >$T/log 2>&1 || bad "$fs partition"
  "$R" format ${L}p1 --fs $fs --label T$fs --real --yes --allow-fixed >$T/log 2>&1 || bad "$fs format: $(tail -2 $T/log)"
  [ "$(blkid -o value -s TYPE ${L}p1)" = $fs ] || bad "$fs: blkid says '$(blkid -o value -s TYPE ${L}p1)'"
  feat=$(dumpe2fs -h ${L}p1 2>/dev/null | grep -i '^Filesystem features')
  case $fs in
    ext2) echo "$feat" | grep -q has_journal && bad "ext2 has a journal";  echo "$feat" | grep -q extent && bad "ext2 has extents" ;;
    ext3) echo "$feat" | grep -q has_journal || bad "ext3 has no journal"; echo "$feat" | grep -q extent && bad "ext3 has extents" ;;
    ext4) echo "$feat" | grep -q has_journal || bad "ext4 has no journal"; echo "$feat" | grep -q extent || bad "ext4 has no extents" ;;
  esac
  losetup -d $L; echo "ok $fs"
done
exit $fail

#!/bin/bash
# extract mode must put the ISO on one big data partition (it used to use a
# 512 MiB ESP, so anything larger failed) typed for Windows, not Linux.
# Needs root, loop devices, sfdisk, mkfs.ntfs, ntfs-3g, 7z, genisoimage, syslinux.
# Usage: test_extract_layout.sh /path/to/rufux      (exit 77 = skipped)
R=$1; HERE=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" = 0 ] || { echo "skip: needs root"; exit 77; }
for t in losetup sfdisk mkfs.ntfs ntfs-3g 7z genisoimage; do
  command -v $t >/dev/null || { echo "skip: $t missing"; exit 77; }
done
[ -e /usr/lib/syslinux/mbr/gptmbr.bin ] || [ -e /usr/share/syslinux/gptmbr.bin ] || { echo "skip: syslinux missing"; exit 77; }
T=$(mktemp -d); chmod 755 $T
cleanup() { umount $T/m 2>/dev/null; losetup -D 2>/dev/null; rm -rf $T; }
trap cleanup EXIT
export PATH=$HERE/shims:$PATH RUFUX_SHIM_MNT=$T/shim
"$HERE/make_fake_winiso.sh" $T/big.iso 600 || exit 77   # 600 MiB payload
truncate -s 2G $T/d.img; L=$(losetup -f --show -P $T/d.img)
if ! "$R" create $T/big.iso $L --mode extract --scheme gpt --fs ntfs --label BIGTEST \
     --real --yes --allow-fixed >$T/log 2>&1; then
  echo "FAIL extract burn"; tail -4 $T/log; exit 1
fi
fail=0; bad() { echo "FAIL $*"; fail=1; }
tab=$(sfdisk -d $L)
[ "$(echo "$tab" | grep -c "^${L}p")" = 1 ] || bad "expected exactly one partition"
echo "$tab" | grep -q EBD0A0A2 || bad "partition is not typed Microsoft basic data"
echo "$tab" | grep -q 0FC63DAF && bad "partition typed as Linux filesystem"
mkdir -p $T/m
if mount -t ntfs-3g ${L}p1 $T/m; then
  sz=$(stat -c %s $T/m/sources/install.wim 2>/dev/null)
  [ "$sz" = $((600*1024*1024)) ] || bad "install.wim is $sz bytes, expected 600 MiB"
  umount $T/m
else bad "cannot mount the data partition"; fi
[ $fail = 0 ] && echo "ok extract"
exit $fail

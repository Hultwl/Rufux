#!/bin/bash
# End-to-end check of Windows install media on a loop device.
# Needs root, loop devices, sfdisk, mkfs.ntfs, ntfs-3g, 7z, genisoimage.
# Usage: test_windows_layout.sh /path/to/rufux      (exit 77 = skipped)
R=$1; HERE=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" = 0 ] || { echo "skip: needs root"; exit 77; }
for t in losetup sfdisk mkfs.ntfs ntfs-3g 7z genisoimage; do
  command -v $t >/dev/null || { echo "skip: $t missing"; exit 77; }
done
T=$(mktemp -d); chmod 755 $T   # 0700 makes FUSE mounts under it look empty
cleanup() { umount $T/m 2>/dev/null; losetup -D 2>/dev/null; rm -rf $T; }
trap cleanup EXIT
export PATH=$HERE/shims:$PATH RUFUX_SHIM_MNT=$T/shim
"$HERE/make_fake_winiso.sh" $T/w.iso 6 || exit 77
mkdir -p $T/m $T/x
7z x -y -o$T/x $T/w.iso >/dev/null
fail=0
bad() { echo "FAIL $*"; fail=1; }

for scheme in gpt dos; do
  truncate -s 1G $T/d.img
  L=$(losetup -f --show -P $T/d.img)
  if ! "$R" create $T/w.iso $L --mode windows --scheme $scheme --label WINTEST \
       --wue nro --real --yes --allow-fixed >$T/log 2>&1; then
    bad "$scheme burn"; tail -5 $T/log; losetup -d $L; continue
  fi
  tab=$(sfdisk -d $L)
  p1=$(echo "$tab" | grep "^${L}p1"); p2=$(echo "$tab" | grep "^${L}p2")
  echo "$p2" | grep -q "size= *2048," || bad "$scheme: partition 2 is not 1 MiB"
  if [ $scheme = gpt ]; then
    echo "$tab" | grep -q C12A7328 && bad "gpt: a partition is typed ESP (Setup breaks with two ESPs)"
    echo "$p1" | grep -q EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 || bad "gpt: data partition is not the real Microsoft basic data GUID"
    echo "$tab" | grep -qi "B938-11D2" && bad "gpt: a partition carries the bogus basic-data GUID (Windows ignores it)"
    grep -q "type 'unknown'" $T/log && bad "gpt: sfdisk reports an unknown partition type"
    echo "$p2" | grep -q 'attrs="GUID:63"' || bad "gpt: partition 2 lacks the no-drive-letter attribute"
  else
    echo "$p1" | grep -q "type=7" || bad "dos: data partition is not type 7"
    echo "$p2" | grep -q "type=ef" || bad "dos: partition 2 is not type ef"
  fi
  cmp -s ${L}p2 "$HERE/../res/uefi/uefi-ntfs.img" || bad "$scheme: partition 2 differs from the UEFI:NTFS image"
  # The NTFS volume must fit its partition, or Windows will not mount it.
  bpb=$(dd if=${L}p1 bs=1 skip=40 count=8 2>/dev/null | od -An -tu8 | tr -d ' ')
  psz=$(blockdev --getsz ${L}p1)
  [ -n "$bpb" ] && [ "$bpb" -le "$psz" ] || bad "$scheme: NTFS boot sector says $bpb sectors but the partition has $psz"
  if mount -t ntfs-3g ${L}p1 $T/m; then
    diff -rq $T/x $T/m --exclude='$OEM$' >$T/diff 2>&1 || { bad "$scheme: tree differs from the ISO"; head -5 $T/diff; }
    grep -q BypassNRO $T/m/sources/'$OEM$'/'$$'/Panther/unattend.xml || bad "$scheme: Panther unattend.xml missing"
    umount $T/m
  else bad "$scheme: cannot mount the data partition"; fi
  ntfsfix -n ${L}p1 >/dev/null 2>&1 || bad "$scheme: NTFS reports problems"
  losetup -d $L
  echo "ok $scheme"
done
exit $fail

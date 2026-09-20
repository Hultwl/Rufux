#!/bin/bash
# FAT32 media needs install.wim split into <4 GiB parts. This exercises the
# splitter (forced with --split-wim on an NTFS stick, because this test must run
# where the kernel may lack vfat) and checks Setup's view: install.swm,
# install2.swm... present, no install.wim, parts reference-able, rest of the
# tree identical. Needs root, loop devices, mkfs.ntfs, ntfs-3g, 7z, genisoimage,
# wimlib-imagex.  Exit 77 = skipped.
R=$1; HERE=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" = 0 ] || { echo "skip: needs root"; exit 77; }
for t in losetup sfdisk mkfs.ntfs ntfs-3g 7z genisoimage wimlib-imagex; do
  command -v $t >/dev/null || { echo "skip: $t missing"; exit 77; }
done
T=$(mktemp -d); chmod 755 $T
cleanup() { umount $T/m 2>/dev/null; losetup -D 2>/dev/null; rm -rf $T; }
trap cleanup EXIT
export PATH=$HERE/shims:$PATH RUFUX_SHIM_MNT=$T/shim RUFUX_TMPDIR=$T/scratch
mkdir -p $T/scratch $T/m $T/x
FAKE_WIM=real "$HERE/make_fake_winiso.sh" $T/w.iso 12 || exit 77
7z x -y -o$T/x $T/w.iso >/dev/null
rm -f /dev/loop0p* 2>/dev/null; losetup -D 2>/dev/null
truncate -s 1G $T/d.img; L=$(losetup -f --show -P $T/d.img)
fail=0; bad() { echo "FAIL $*"; fail=1; }
if ! "$R" create $T/w.iso $L --mode windows --scheme gpt --label SPLIT --split-wim 4 \
     --real --yes --allow-fixed >$T/log 2>&1; then bad "burn"; tail -5 $T/log; exit 1; fi
grep -q "Splitting install.wim" $T/log || bad "the split was not announced"
mount -t ntfs-3g ${L}p1 $T/m || { bad "cannot mount"; exit 1; }
S=$T/m/sources
[ -f $S/install.swm ] || bad "install.swm missing"
[ ! -e $S/install.wim ] || bad "install.wim was copied unsplit"
parts=$(ls $S/install*.swm 2>/dev/null | wc -l)
[ "$parts" -ge 3 ] || bad "expected at least 3 parts for 12 MiB at 4 MiB, got $parts"
for f in $S/install*.swm; do [ "$(stat -c %s $f)" -le $((5*1024*1024)) ] || bad "$(basename $f) is larger than the part size"; done
wimlib-imagex info $S/install.swm >$T/info 2>&1 && grep -q "Fake Windows" $T/info || { bad "split WIM does not read back as one image"; head -6 $T/info; }
wimlib-imagex verify $S/install.swm --ref="$S/install*.swm" >/dev/null 2>&1 || bad "split WIM does not verify"
diff -rq $T/x $T/m --exclude='install*.wim' --exclude='install*.swm' >$T/diff 2>&1 || { bad "rest of the tree differs"; head -3 $T/diff; }
grep -q "Verified sources/install.swm" $T/log || bad "the tree check did not accept install.swm"
umount $T/m
[ -z "$(ls $T/scratch)" ] || bad "scratch files left behind: $(ls $T/scratch)"
[ $fail = 0 ] && echo "ok split"
exit $fail

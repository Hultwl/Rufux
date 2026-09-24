#!/bin/bash
# Virtual disk images (QCOW2, VMDK, VDI, VHDX, dynamic VHD) are expanded onto the
# target exactly, fixed VHD keeps working, and images that reference other files
# are refused. Needs qemu-img, root and loop devices for the write tests.
# Usage: test_vdisk.sh /path/to/rufux      (exit 77 = skipped)
R=$1; HERE=$(cd "$(dirname "$0")" && pwd)
command -v qemu-img >/dev/null || { echo "skip: qemu-img missing"; exit 77; }
[ "$(id -u)" = 0 ] || { echo "skip: needs root"; exit 77; }
command -v losetup >/dev/null || { echo "skip: losetup missing"; exit 77; }
T=$(mktemp -d); trap 'losetup -D 2>/dev/null; rm -rf $T' EXIT
fail=0; bad() { echo "FAIL $*"; fail=1; }
head -c 8388608 /dev/urandom > $T/raw.img
truncate -s 32M $T/d.img; L=$(losetup -f --show $T/d.img)
# same-size compare helper: first 8 MiB of the target must equal the source data
same() { cmp -n 8388608 $T/raw.img $L; }

for f in qcow2 vmdk vdi vhdx vpc; do
  qemu-img convert -f raw -O $f $T/raw.img $T/i.$f
  dd if=/dev/zero of=$L bs=1M count=32 2>/dev/null
  if ! "$R" create $T/i.$f $L --mode dd --real --yes --allow-fixed --verify >$T/log 2>&1; then
    bad "$f: write failed: $(tail -2 $T/log)"; continue; fi
  same || bad "$f: content on the target differs from the source"
  grep -q "converting with qemu-img" $T/log || bad "$f: did not take the qemu-img path"
  # the part of the drive beyond the image must be left alone
  [ "$(dd if=$L bs=1M skip=8 2>/dev/null | tr -d '\0' | wc -c)" = 0 ] || bad "$f: wrote past the end of the image"
  echo "ok $f"
done

# a fixed VHD is a raw payload with a footer and must not go through qemu-img
qemu-img convert -f raw -O vpc -o subformat=fixed $T/raw.img $T/fixed.vhd
dd if=/dev/zero of=$L bs=1M count=32 2>/dev/null
"$R" create $T/fixed.vhd $L --mode dd --real --yes --allow-fixed >$T/log 2>&1 || bad "fixed vhd: $(tail -2 $T/log)"
grep -q "Fixed VHD detected" $T/log || bad "fixed vhd: not recognised"
grep -q "qemu-img" $T/log && bad "fixed vhd: went through qemu-img"
same || bad "fixed vhd: content differs"; echo "ok fixed-vhd"

# a plain ISO/raw image is untouched by all this
dd if=/dev/zero of=$L bs=1M count=32 2>/dev/null
"$R" create $T/raw.img $L --mode dd --real --yes --allow-fixed >$T/log 2>&1 || bad "raw: $(tail -2 $T/log)"
same || bad "raw: content differs"; grep -q qemu-img $T/log && bad "raw image went through qemu-img"; echo "ok raw"

# too big for the drive
qemu-img create -f qcow2 $T/big.qcow2 1G >/dev/null
"$R" create $T/big.qcow2 $L --mode dd --real --yes --allow-fixed >$T/log 2>&1 && bad "an oversized image was accepted"
grep -q "holds only" $T/log || bad "oversized: wrong message: $(tail -1 $T/log)"; echo "ok too-big"

# a QCOW2 with a backing file would copy that file's contents onto the drive
echo "secret" > $T/secret.bin; truncate -s 8M $T/secret.bin
qemu-img create -f qcow2 -b $T/secret.bin -F raw $T/backed.qcow2 >/dev/null
"$R" create $T/backed.qcow2 $L --mode dd --real --yes --allow-fixed >$T/log 2>&1 && bad "a backing-file image was written"
grep -q "refers to another file" $T/log || bad "backing file: wrong message: $(tail -1 $T/log)"; echo "ok backing-file"

# a VMDK descriptor whose extent is an arbitrary file elsewhere is refused
mkdir -p $T/x && cat > $T/x/evil.vmdk <<E
# Disk DescriptorFile
version=1
CID=fffffffe
parentCID=ffffffff
createType="monolithicFlat"
RW 16384 FLAT "/etc/passwd" 0
E
"$R" create $T/x/evil.vmdk $L --mode dd --real --yes --allow-fixed >$T/log 2>&1 && bad "a VMDK pointing at /etc/passwd was written"
grep -qE "refers to another file|cannot read" $T/log || bad "evil vmdk: wrong message: $(tail -1 $T/log)"; echo "ok evil-vmdk"

# the legitimate split form (descriptor + flat extent next to it) works
qemu-img convert -f raw -O vmdk -o subformat=monolithicFlat $T/raw.img $T/x/flat.vmdk
dd if=/dev/zero of=$L bs=1M count=32 2>/dev/null
"$R" create $T/x/flat.vmdk $L --mode dd --real --yes --allow-fixed >$T/log 2>&1 || bad "flat vmdk: $(tail -2 $T/log)"
same || bad "flat vmdk: content differs"; echo "ok flat-vmdk"

# a corrupted image must not be reported as written
cp $T/i.qcow2 $T/trunc.qcow2; truncate -s 100000 $T/trunc.qcow2
dd if=/dev/zero of=$L bs=1M count=32 2>/dev/null
"$R" create $T/trunc.qcow2 $L --mode dd --real --yes --allow-fixed >$T/log 2>&1
rc=$?; [ $rc = 0 ] && bad "a truncated qcow2 reported success"; echo "ok truncated (rc=$rc)"

# a dynamic VHD cut short (footer copy at the end gone) is refused, not written as raw bytes
cp $T/i.vpc $T/trunc.vhd; truncate -s $(( $(stat -c %s $T/i.vpc) - 512 )) $T/trunc.vhd
"$R" create $T/trunc.vhd $L --mode dd --real --yes --allow-fixed >$T/log 2>&1 && bad "a truncated dynamic VHD was written"
grep -q "truncated or damaged" $T/log || bad "truncated vhd: wrong message: $(tail -1 $T/log)"; echo "ok truncated-vhd"

# the plain "write" command takes the same path
dd if=/dev/zero of=$L bs=1M count=32 2>/dev/null
"$R" write $T/i.vhdx $L --real --yes --allow-fixed --verify >$T/log 2>&1 || bad "write cmd: $(tail -2 $T/log)"
same || bad "write cmd: content differs"; echo "ok write-cmd"

# dry run changes nothing
dd if=/dev/zero of=$L bs=1M count=32 2>/dev/null
"$R" create $T/i.qcow2 $L --mode dd --dry-run --allow-fixed >$T/log 2>&1 || bad "dry-run failed"
[ "$(tr -d '\0' < $L | wc -c)" = 0 ] || bad "dry-run wrote to the drive"
# missing qemu-img gives a clear instruction, not a raw-copy of the container
mkdir -p $T/nobin; for t in $(ls /usr/bin | head -0); do :; done
PATH=$T/nobin "$R" create $T/i.qcow2 $L --mode dd --real --yes --allow-fixed >$T/log 2>&1 && bad "no qemu-img: accepted"
grep -q "needs qemu-img" $T/log || bad "no qemu-img: wrong message: $(tail -1 $T/log)"; echo "ok no-qemu-img"
losetup -d $L
exit $fail

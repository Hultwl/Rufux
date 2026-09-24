#!/bin/bash
# SMART gate: a failing drive is refused, everything else (passed, warnings,
# no SMART data at all, USB-SATA bridges) is allowed, and --ignore-smart overrides.
# Needs root, loop devices, sfdisk, mkfs.vfat.  Usage: test_smart.sh /path/to/rufux  (77 = skipped)
R=$1; HERE=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" = 0 ] || { echo "skip: needs root"; exit 77; }
for t in losetup sfdisk mkfs.vfat; do command -v $t >/dev/null || { echo "skip: $t missing"; exit 77; }; done
T=$(mktemp -d); trap 'losetup -D 2>/dev/null; rm -rf $T' EXIT
export PATH=$HERE/shims:$PATH
fail=0; bad() { echo "FAIL $*"; fail=1; }
truncate -s 128M $T/d.img; L=$(losetup -f --show -P $T/d.img)

run() { RUFUX_SHIM_SMART=$1 "$R" create none $L --mode format --scheme dos --fs vfat --real --yes --allow-fixed "${@:2}" >$T/log 2>&1; }

run pass;     [ $? = 0 ] || bad "pass: write refused: $(tail -1 $T/log)"; grep -q "self-assessment passed" $T/log || bad "pass: no verdict logged"
run warn;     [ $? = 0 ] || bad "warn: write refused"; grep -q "reallocated 12, pending 3" $T/log || bad "warn: bad sectors not reported: $(grep SMART $T/log)"
run none;     [ $? = 0 ] || bad "none: write refused"; grep -q "does not report health data" $T/log || bad "none: not explained"
run sat-only; [ $? = 0 ] || bad "sat-only: write refused"; grep -q "self-assessment passed" $T/log || bad "sat-only: -d sat fallback not used"
# start the failing-drive case from a blank disk so "untouched" can be checked
wipefs -a -f $L >/dev/null; dd if=/dev/zero of=$L bs=1M count=4 2>/dev/null; partprobe $L 2>/dev/null
run fail;     [ $? != 0 ] || bad "fail: a failing drive was written"; grep -q "FAILING" $T/log || bad "fail: no explanation"; grep -q "ignore-smart" $T/log || bad "fail: no override hint"
# refused means untouched: no partition was created
[ -z "$(sfdisk -d $L 2>/dev/null)" ] || bad "fail: the drive was modified anyway"
run fail --ignore-smart; [ $? = 0 ] || bad "fail + --ignore-smart: still refused: $(tail -1 $T/log)"
# standalone command
RUFUX_SHIM_SMART=fail "$R" smart $L >$T/log 2>&1; [ $? = 4 ] || bad "smart cmd: exit code for a failing drive is not 4"
RUFUX_SHIM_SMART=pass "$R" smart $L >$T/log 2>&1; [ $? = 0 ] || bad "smart cmd: passing drive should exit 0"
# a regular file is not a drive: skipped, never an error
"$R" smart $T/d.img >$T/log 2>&1; grep -q "not a block device" $T/log || bad "file: not skipped"
# dry-run never calls smartctl
RUFUX_SHIM_SMART=fail "$R" create none $L --mode format --scheme dos --fs vfat --dry-run --allow-fixed >$T/log 2>&1 || bad "dry-run refused"
# the real smartctl (no shim) on a loop device must be harmless
PATH=${PATH#$HERE/shims:} "$R" smart $L >$T/log 2>&1; [ $? = 0 ] || bad "real smartctl on a loop device: $(cat $T/log)"
losetup -d $L
[ $fail = 0 ] && echo "ok smart"
exit $fail

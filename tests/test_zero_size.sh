#!/bin/bash
# A target that reports 0 bytes (empty card-reader slot, unreadable stick)
# must be refused with an explanation, not fail later with "too small".
# An unattached loop device has capacity 0, which stands in for the slot.
# Needs root and a free loop device.  Exit 77 = skipped.
R=$1
[ "$(id -u)" = 0 ] || { echo "skip: needs root"; exit 77; }
command -v losetup >/dev/null || { echo "skip: losetup missing"; exit 77; }
free=$(losetup -f 2>/dev/null) || { echo "skip: no free loop device"; exit 77; }
[ -b "$free" ] || { echo "skip: $free is not a block device"; exit 77; }
out=$("$R" partition "$free" --scheme gpt --layout single --dry-run --allow-fixed 2>&1)
echo "$out" | grep -q "reports a capacity of 0 bytes" || { echo "FAIL zero-size target not explained: $out"; exit 1; }
echo "ok zero-size"

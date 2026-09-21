#!/bin/bash
# Command line strictness, and the window's option paths (no root needed).
#  1. an unknown option is an error (exit 2), never silently ignored
#  2. a close typo gets a suggestion, and nothing is done
#  3. an option that needs a value and has none is an error
#  4. every argument set the window can send is accepted by the strict parser
#  5. /dev/disk/by-id style symlinks resolve to the real device (needs root + loop)
R=$1; HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d); trap 'rm -rf $T' EXIT
fail=0; bad() { echo "FAIL $*"; fail=1; }
"$HERE/make_fake_winiso.sh" $T/w.iso 2 >/dev/null 2>&1 || { echo "skip: cannot build test ISO"; exit 77; }
truncate -s 64M $T/d.img

# 1-3
for c in "list" "probe $T/w.iso" "checksum $T/w.iso" "write $T/w.iso $T/d.img" "partition $T/d.img" \
         "format $T/d.img" "install-boot $T/d.img" "create $T/w.iso $T/d.img --mode windows"; do
  "$R" $c --definitely-not-an-option >$T/o 2>&1; rc=$?
  [ $rc = 2 ] || bad "'$c': unknown option gave exit $rc, expected 2"
  grep -q "unknown option" $T/o || bad "'$c': no 'unknown option' message"
done
"$R" create $T/w.iso $T/d.img --mode windows --rela --dry-run --allow-file >$T/o 2>&1
[ $? = 2 ] && grep -q "did you mean '--real'" $T/o || bad "typo --rela: no suggestion"
"$R" create $T/w.iso $T/d.img --mode >$T/o 2>&1
[ $? = 2 ] && grep -q "needs a value" $T/o || bad "--mode without a value was accepted"
"$R" create $T/w.iso $T/d.img --mode windows --dry-run --allow-file >/dev/null 2>&1 || bad "a valid command was rejected"

# 4: what the window sends (see onStart in gui_qt.cpp)
n=0
for mode in windows extract dd; do
 for scheme in gpt dos; do
  for fs in vfat ntfs exfat udf ext4; do
   case $mode-$fs in windows-exfat|windows-udf|windows-ext4) continue;; esac
   for cluster in 0 2 16 128; do
    for quick in --quick --full; do
     args=(create $T/w.iso $T/d.img --mode $mode --scheme $scheme --fs $fs --label TEST
           --persist-mb $([ $mode = extract ] && echo 512 || echo 0) --cluster-sectors $cluster
           --badblock-passes 1 $quick --no-autorun --verify --allow-fixed --dry-run --allow-file)
     [ $mode = windows ] && args+=(--wue bypass,nro,privacy,bitlocker,qol,locale,user=Sam --locale en-US --timezone Africa/Cairo)
     "$R" "${args[@]}" >$T/o 2>&1 || { bad "window args rejected: ${args[*]:3}"; tail -2 $T/o; break 5; }
     n=$((n+1))
    done
   done
  done
 done
done
for mode in format dos; do
  "$R" create none $T/d.img --mode $mode --scheme dos --fs vfat --label TEST --cluster-sectors 0 --quick --dry-run --allow-file >$T/o 2>&1 || bad "window args for mode $mode rejected"
done
echo "checked $n window argument sets"

# 5: by-id style symlink (needs root and a loop device)
if [ "$(id -u)" = 0 ] && command -v losetup >/dev/null; then
  L=$(losetup -f --show $T/d.img 2>/dev/null)
  if [ -n "$L" ]; then
    mkdir -p /dev/disk/by-id; ln -sf $L /dev/disk/by-id/usb-Rufux_Test-0:0
    "$R" partition /dev/disk/by-id/usb-Rufux_Test-0:0 --scheme gpt --layout single --dry-run --allow-fixed >$T/o 2>&1
    grep -q "partition $L as" $T/o || bad "by-id symlink was not resolved to $L"
    rm -f /dev/disk/by-id/usb-Rufux_Test-0:0; losetup -d $L
  fi
fi
[ $fail = 0 ] && echo "ok cli"
exit $fail

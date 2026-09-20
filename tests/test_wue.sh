#!/bin/bash
# Windows User Experience: registry bypass inside boot.wim, answer-file
# placement, driver injection, and the fallback when wimlib fails.
# Needs root, loop devices, sfdisk, mkfs.ntfs, ntfs-3g, 7z, genisoimage,
# wimlib-imagex, hivexsh, hivexget, python3.  Exit 77 = skipped.
R=$1; HERE=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" = 0 ] || { echo "skip: needs root"; exit 77; }
for t in losetup sfdisk mkfs.ntfs ntfs-3g 7z genisoimage wimlib-imagex hivexsh hivexget python3; do
  command -v $t >/dev/null || { echo "skip: $t missing"; exit 77; }
done
T=$(mktemp -d); chmod 755 $T
cleanup() { umount $T/m 2>/dev/null; losetup -D 2>/dev/null; rm -rf $T; }
trap cleanup EXIT
export PATH=$HERE/shims:$PATH RUFUX_SHIM_MNT=$T/shim
"$HERE/make_fake_winiso.sh" $T/w.iso 6 || exit 77
mkdir -p $T/drv/net $T/m
echo "[Version]" > $T/drv/iaStorVD.inf; echo x > $T/drv/iaStorVD.sys; echo "[Version]" > $T/drv/net/e1d.inf
fail=0; bad() { echo "FAIL $*"; fail=1; }

burn() {  # burn ARGS...   (fresh 1 GiB disk each time)
  rm -f /dev/loop0p* 2>/dev/null; losetup -D 2>/dev/null
  truncate -s 1G $T/d.img; L=$(losetup -f --show -P $T/d.img)
  "$R" create $T/w.iso $L --mode windows --scheme gpt --label WUETEST --real --yes --allow-fixed "$@" >$T/log 2>&1
}
hive_val() {  # hive_val WIM INDEX  -> LabConfig BypassTPMCheck value or empty
  local d=$(mktemp -d -p $T)
  wimlib-imagex extract "$1" "$2" /Windows/System32/config/SYSTEM --dest-dir=$d --no-acls >/dev/null 2>&1
  hivexget $d/SYSTEM '\Setup\LabConfig' BypassTPMCheck 2>/dev/null
}

# 1. everything on, wimlib + hivex available: the "under the hood" path
if burn --wue bypass,nro,privacy --drivers $T/drv; then
  mount -t ntfs-3g ${L}p1 $T/m || bad "cannot mount"
  [ ! -e $T/m/autounattend.xml ] || bad "root autounattend.xml exists (bypass should be in boot.wim)"
  P=$T/m/sources/'$OEM$'/'$$'/Panther/unattend.xml
  [ -f "$P" ] || bad "no sources/\$OEM\$/\$\$/Panther/unattend.xml"
  grep -q BypassNRO "$P" || bad "unattend.xml lacks BypassNRO"
  grep -q ProtectYourPC "$P" || bad "unattend.xml lacks the privacy settings"
  grep -q 'pass="windowsPE"' "$P" && bad "unattend.xml has a windowsPE pass (changes Setup's screens)"
  [ "$(hive_val $T/m/sources/boot.wim 2)" = 1 ] || bad "boot.wim image 2 lacks LabConfig\\BypassTPMCheck=1"
  [ -z "$(hive_val $T/m/sources/boot.wim 1)" ] || bad "boot.wim image 1 was modified"
  wimlib-imagex verify $T/m/sources/boot.wim >/dev/null 2>&1 || bad "boot.wim no longer verifies"
  [ -f $T/m/'$WinPEDriver$'/iaStorVD.inf ] && [ -f $T/m/'$WinPEDriver$'/net/e1d.inf ] || bad "drivers not copied to \$WinPEDriver\$"
  umount $T/m
  echo "ok under-the-hood"
else bad "burn (all options)"; tail -4 $T/log; fi

# 2. wimlib unusable: fall back to the answer file, and say so
mkdir -p $T/badbin; printf '#!/bin/sh\nexit 1\n' > $T/badbin/wimlib-imagex; chmod +x $T/badbin/wimlib-imagex
if PATH=$T/badbin:$PATH burn --wue bypass; then
  mount -t ntfs-3g ${L}p1 $T/m || bad "cannot mount (fallback)"
  grep -q "BypassSecureBootCheck" $T/m/autounattend.xml 2>/dev/null || bad "fallback: no autounattend.xml with the bypass"
  grep -q 'pass="windowsPE"' $T/m/autounattend.xml 2>/dev/null || bad "fallback: no windowsPE pass"
  umount $T/m
  grep -q "fallback" $T/log || bad "fallback was not reported in the log"
  echo "ok fallback"
else bad "burn (fallback)"; fi

# 3. only 'nro': no windowsPE pass and boot.wim untouched
if burn --wue nro; then
  mount -t ntfs-3g ${L}p1 $T/m
  [ -z "$(hive_val $T/m/sources/boot.wim 2)" ] || bad "nro-only run modified boot.wim"
  [ -f $T/m/sources/'$OEM$'/'$$'/Panther/unattend.xml ] || bad "nro-only: no Panther unattend.xml"
  [ ! -e $T/m/autounattend.xml ] || bad "nro-only: unexpected root autounattend.xml"
  umount $T/m; echo "ok nro-only"
else bad "burn (nro)"; fi

# 4. --drivers must be a folder that really holds drivers
mkdir -p $T/empty
burn --wue none --drivers $T/empty && bad "empty drivers folder accepted"
grep -q "no .inf driver files" $T/log || bad "no clear error for an empty drivers folder"
echo "ok drivers-validation"
exit $fail

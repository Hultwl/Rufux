#!/bin/bash
# Windows User Experience: registry bypass inside boot.wim, answer-file
# placement, the other options, and the fallback when wimlib fails.
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
mkdir -p $T/m
fail=0; bad() { echo "FAIL $*"; fail=1; }

burn() {  # burn ARGS...   (fresh 1 GiB disk each time)
  rm -f /dev/loop0p* 2>/dev/null; losetup -D 2>/dev/null
  rm -f $T/d.img; truncate -s 1G $T/d.img; L=$(losetup -f --show -P $T/d.img)
  "$R" create $T/w.iso $L --mode windows --scheme gpt --label WUETEST --real --yes --allow-fixed "$@" >$T/log 2>&1
}
hive_val() {  # hive_val WIM INDEX  -> LabConfig BypassTPMCheck value or empty
  local d=$(mktemp -d -p $T)
  wimlib-imagex extract "$1" "$2" /Windows/System32/config/SYSTEM --dest-dir=$d --no-acls >/dev/null 2>&1
  hivexget $d/SYSTEM '\Setup\LabConfig' BypassTPMCheck 2>/dev/null
}

# 1. everything on, wimlib + hivex available: the "under the hood" path
if burn --wue bypass,nro,privacy; then
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
  umount $T/m
  echo "ok under-the-hood"
else bad "burn (all options)"; tail -4 $T/log; fi

# 2. wimlib unusable: fall back to the answer file, and say so
mkdir -p $T/badbin; printf '#!/bin/sh\nexit 1\n' > $T/badbin/wimlib-imagex; chmod +x $T/badbin/wimlib-imagex
if PATH=$T/badbin:$PATH burn --wue bypass; then
  mount -t ntfs-3g ${L}p1 $T/m || bad "cannot mount (fallback)"
  grep -q "BypassSecureBootCheck" $T/m/autounattend.xml 2>/dev/null || bad "fallback: no autounattend.xml with the bypass"
  grep -q 'pass="windowsPE"' $T/m/autounattend.xml 2>/dev/null || bad "fallback: no windowsPE pass"
  grep -q "BypassTPMCheck" $T/m/autounattend.xml && grep -q "BypassRAMCheck" $T/m/autounattend.xml || bad "fallback: TPM/RAM values missing"
  grep -qE "BypassCPUCheck|BypassStorageCheck" $T/m/autounattend.xml && bad "fallback: writes bypass values Rufus does not have"
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


# 5. local account, regional options, BitLocker, QoL: contents and well-formedness
if burn --wue bypass,nro,privacy,bitlocker,qol,locale,user=Hus:sein --locale ar-EG --keyboard 0401:00000401 --timezone Africa/Cairo; then
  mount -t ntfs-3g ${L}p1 $T/m || bad "cannot mount (options)"
  P=$T/m/sources/'$OEM$'/'$$'/Panther/unattend.xml
  python3 -c "import sys,xml.dom.minidom as m; m.parse(sys.argv[1])" "$P" 2>/dev/null || bad "unattend.xml is not well-formed XML"
  grep -q '<Name>Hus_sein</Name>' "$P" || bad "local account name missing or not sanitised"
  grep -q '<PlainText>false</PlainText>' "$P" || bad "account password block missing"
  grep -q 'net user "Hus_sein" /logonpasswordchg:yes' "$P" || bad "first-logon password change missing"
  grep -q '<TimeZone>Egypt Standard Time</TimeZone>' "$P" || bad "time zone not mapped"
  grep -q '<InputLocale>0401:00000401</InputLocale>' "$P" || bad "keyboard layout missing"
  grep -q '<UILanguage>ar-EG</UILanguage>' "$P" || bad "language missing"
  grep -q 'PreventDeviceEncryption' "$P" || bad "BitLocker option missing"
  grep -q 'HiberbootEnabled' "$P" || bad "QoL commands missing"
  grep -q 'pass="windowsPE"' "$P" && bad "windowsPE pass present although bypass went into boot.wim"
  umount $T/m; echo "ok options"
else bad "burn (options)"; tail -3 $T/log; fi

# 6. reserved and empty account names are refused before anything is written
burn --wue user=Administrator && bad "reserved account name accepted"
[ -z "$(sfdisk -d ${L} 2>/dev/null | grep "^${L}p")" ] || bad "disk was partitioned before the option error"
grep -q "not allowed as a local account name" $T/log || bad "no clear error for a reserved name"
echo "ok user-validation"
exit $fail

#!/bin/bash
# Bootloader revocation check, offline and deterministic: builds tiny EFI images,
# signs them with a throwaway key (sbsign), computes their Authenticode hash with an
# independent implementation (mkpe.py) and builds DBX files that revoke them.
# Usage: test_bootcheck.sh /path/to/rufux      (exit 77 = skipped)
R=$1; HERE=$(cd "$(dirname "$0")" && pwd)
for t in sbsign openssl python3; do command -v $t >/dev/null || { echo "skip: $t missing"; exit 77; }; done
T=$(mktemp -d); trap 'rm -rf $T' EXIT
fail=0; bad() { echo "FAIL $*"; fail=1; }
M="python3 $HERE/mkpe.py"
openssl req -new -x509 -newkey rsa:2048 -nodes -keyout $T/k.pem -out $T/c.pem -days 2 -subj "/CN=Rufux Test CA" 2>/dev/null
openssl x509 -in $T/c.pem -outform DER -out $T/c.der
sign() { sbsign --key $T/k.pem --cert $T/c.pem --output "$2" "$1" >/dev/null 2>&1; }
state() { RUFUX_DBX_DIR=$T/dbx "$R" check-boot "$1" 2>&1 | head -1 | sed 's/^[^:]*: \([a-z-]*\).*/\1/'; }
mkdir -p $T/dbx
$M dbx $T/dbx/dbx_x64.efiauth2                      # empty DBX
$M pe $T/plain.efi; $M pe $T/ok.pre "sbat,1,2025051000
grub,5,Free Software Foundation,grub,2.12,https://www.gnu.org/software/grub/"
$M pe $T/oldgrub.pre "sbat,1,2025051000
grub,1,Free Software Foundation,grub,2.02,https://www.gnu.org/software/grub/"
$M pe $T/oldshim.pre "sbat,1,2025051000
shim,3,UEFI shim,shim,15.4,https://github.com/rhboot/shim"
$M pe $T/nosbat.pre
sign $T/ok.pre $T/ok.efi; sign $T/oldgrub.pre $T/oldgrub.efi; sign $T/oldshim.pre $T/oldshim.efi; sign $T/nosbat.pre $T/nosbat.efi

[ "$(state $T/plain.efi)" = unsigned ] || bad "unsigned image reported as '$(state $T/plain.efi)'"
[ "$(state $T/ok.efi)" = ok ] || bad "signed image with current SBAT reported as '$(state $T/ok.efi)'"
[ "$(state $T/nosbat.efi)" = ok ] || bad "signed image without .sbat reported as '$(state $T/nosbat.efi)'"
[ "$(state $T/oldgrub.efi)" = revoked-sbat ] || bad "grub,1 not revoked by SBAT: '$(state $T/oldgrub.efi)'"
[ "$(state $T/oldshim.efi)" = revoked-sbat ] || bad "shim,3 not revoked by SBAT: '$(state $T/oldshim.efi)'"
RUFUX_DBX_DIR=$T/dbx "$R" check-boot $T/oldgrub.efi | grep -q "grub' generation 1 is below the current minimum 5" || bad "SBAT message lacks the numbers"
echo "ok signed/sbat"

# our hash must equal the independent one, and the DBX must catch exactly that hash
h=$($M hash $T/ok.efi); [ "$($M hash $T/ok.pre)" = "$h" ] || bad "Authenticode hash changed by signing (test helper broken)"
$M dbx $T/dbx/dbx_x64.efiauth2 --sha $h
[ "$(state $T/ok.efi)" = revoked-dbx ] || bad "DBX hash entry not honoured: '$(state $T/ok.efi)'"
[ "$(state $T/nosbat.efi)" = ok ] || bad "an unrelated image is revoked by a one-entry DBX"
$M dbx $T/dbx/dbx_x64.efiauth2 --sha $(printf '%064x' 1) --sha $h --sha $(printf '%064x' 2)
[ "$(state $T/ok.efi)" = revoked-dbx ] || bad "hash in the middle of a DBX list missed"
echo "ok dbx-hash"

# certificate revocation: the DBX lists the TBS hash of a certificate in the chain
$M dbx $T/dbx/dbx_x64.efiauth2 --tbs $($M tbs $T/c.der)
[ "$(state $T/ok.efi)" = revoked-dbx ] || bad "DBX certificate entry not honoured: '$(state $T/ok.efi)'"
RUFUX_DBX_DIR=$T/dbx "$R" check-boot $T/ok.efi | grep -q "certificate" || bad "cert revocation message"
$M dbx $T/dbx/dbx_x64.efiauth2 --tbs $(printf '%064x' 5)
[ "$(state $T/ok.efi)" = ok ] || bad "a foreign certificate entry revoked an unrelated image"
echo "ok dbx-cert"

# missing / truncated DBX must not crash or revoke anything
rm $T/dbx/dbx_x64.efiauth2; [ "$(state $T/ok.efi)" = ok ] || bad "no DBX file: '$(state $T/ok.efi)'"
head -c 40 /dev/urandom > $T/dbx/dbx_x64.efiauth2; [ "$(state $T/ok.efi)" = ok ] || bad "garbage DBX: '$(state $T/ok.efi)'"
# hostile PE files: never crash
for n in 0 64 200 1000 3000; do head -c $n $T/ok.efi > $T/cut.efi; RUFUX_DBX_DIR=$T/dbx "$R" check-boot $T/cut.efi >/dev/null 2>&1; [ $? -lt 128 ] || bad "crash on a $n byte truncation"; done
for i in 1 2 3 4 5 6 7 8; do head -c 4096 /dev/urandom > $T/r.efi; printf 'MZ' | dd of=$T/r.efi conv=notrunc 2>/dev/null; RUFUX_DBX_DIR=$T/dbx "$R" check-boot $T/r.efi >/dev/null 2>&1; [ $? -lt 128 ] || bad "crash on random MZ file"; done
[ "$(state $HERE/../res/uefi/uefi-ntfs.img)" = not-efi ] || bad "a disk image is not reported as not-efi"
echo "ok robustness"

# directory walk: finds loaders in any case and nested, exit code 4 when one is revoked
mkdir -p $T/tree/EFI/BOOT $T/tree/efi/microsoft/boot
$M dbx $T/dbx/dbx_x64.efiauth2 --sha $h
cp $T/ok.efi $T/tree/EFI/BOOT/BOOTX64.EFI; cp $T/nosbat.efi $T/tree/efi/microsoft/boot/bootmgfw.efi; echo hi > $T/tree/readme.txt
RUFUX_DBX_DIR=$T/dbx "$R" check-boot $T/tree > $T/out 2>&1; rc=$?
[ $rc = 4 ] || bad "tree with a revoked loader exits $rc, not 4"
grep -q "WARNING: revoked UEFI bootloader 'BOOTX64.EFI'" $T/out || bad "tree: revoked loader not named: $(cat $T/out)"
grep -q "bootmgfw.efi' (x64): signed by 'Rufux Test CA', not revoked" $T/out || bad "tree: good loader not reported"
rm $T/tree/EFI/BOOT/BOOTX64.EFI; RUFUX_DBX_DIR=$T/dbx "$R" check-boot $T/tree >/dev/null 2>&1 || bad "clean tree should exit 0"
echo "ok tree"
exit $fail

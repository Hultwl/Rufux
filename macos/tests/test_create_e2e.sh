#!/usr/bin/env bash
set -euo pipefail

MACOS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT_DIR="$(cd "${MACOS_DIR}/.." && pwd)"
RUFUX="${1:-${MACOS_DIR}/build/rufux}"

export PATH="${MACOS_DIR}/vendor/ntfs-3g/sbin:${MACOS_DIR}/vendor/ntfs-3g/bin:/opt/homebrew/opt/e2fsprogs/sbin:/opt/homebrew/opt/e2fsprogs/bin:/opt/homebrew/bin:/usr/sbin:/sbin:/usr/bin:/bin:${PATH}"
export RUFUX_NO_GUI_HANDOFF=1

TMP="$(mktemp -d /tmp/rufux-test-create-e2e-XXXXXX)"
MNT_ROOT="${TMP}/mnt_root"
STATE_DIR="${TMP}/mnt_state"
TMP_SCRATCH="${TMP}/scratch"
mkdir -p "${MNT_ROOT}" "${STATE_DIR}" "${TMP_SCRATCH}"

export RUFUX_MOUNT_ROOT="${MNT_ROOT}"
export RUFUX_MOUNT_STATE_DIR="${STATE_DIR}"
export RUFUX_TMPDIR="${TMP_SCRATCH}"

TRACKED_MNTS_FILE="${TMP}/active_mnts.list"
TRACKED_DISKS_FILE="${TMP}/attached_disks.list"
: > "${TRACKED_MNTS_FILE}"
: > "${TRACKED_DISKS_FILE}"

cleanup() {
  local rc=$?
  set +e
  if [ -f "${TRACKED_MNTS_FILE}" ]; then
    while IFS= read -r m; do
      if [ -n "${m}" ]; then
        if [ -x "${RUFUX}" ]; then
          "${RUFUX}" umount "${m}" >/dev/null 2>&1 || true
        fi
        /sbin/umount -f "${m}" >/dev/null 2>&1 || true
        /usr/sbin/diskutil unmount force "${m}" >/dev/null 2>&1 || true
      fi
    done < "${TRACKED_MNTS_FILE}"
  fi
  if [ -d "${MNT_ROOT}" ]; then
    for d in "${MNT_ROOT}"/*; do
      if [ -d "${d}" ]; then
        /sbin/umount -f "${d}" >/dev/null 2>&1 || true
        /usr/sbin/diskutil unmount force "${d}" >/dev/null 2>&1 || true
      fi
    done
  fi
  if [ -d "${STATE_DIR}" ]; then
    for pf in "${STATE_DIR}"/*.pid "${STATE_DIR}"/*.state; do
      if [ -f "${pf}" ]; then
        pid="$(grep -E '^[0-9]+$' "${pf}" 2>/dev/null | head -n 1 || true)"
        if [ -z "${pid}" ]; then
          pid="$(awk -F= '/^pid=/{print $2}' "${pf}" 2>/dev/null | head -n 1 || true)"
        fi
        if [ -n "${pid}" ]; then
          kill -TERM "${pid}" >/dev/null 2>&1 || true
        fi
      fi
    done
  fi
  if [ -f "${TRACKED_DISKS_FILE}" ]; then
    while IFS= read -r dk; do
      if [ -n "${dk}" ]; then
        /usr/bin/hdiutil detach -force "${dk}" >/dev/null 2>&1 || true
      fi
    done < "${TRACKED_DISKS_FILE}"
  fi
  rm -rf "${TMP}"
  exit "${rc}"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

if [ ! -x "${RUFUX}" ]; then
  fail "rufux CLI binary not built at ${RUFUX}"
fi

attach_raw_rw() {
  local img="$1"
  local out dk
  out="$(/usr/bin/hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount -readwrite "${img}")"
  dk="$(echo "${out}" | awk '/^\/dev\/disk[0-9]+/{print $1; exit}')"
  [ -n "${dk}" ] || fail "failed to attach raw disk image ${img}"
  echo "${dk}" >> "${TRACKED_DISKS_FILE}"
  echo "${dk}"
}

detach_raw() {
  local dk="$1"
  /usr/bin/hdiutil detach -force "${dk}" >/dev/null
  if [ -f "${TRACKED_DISKS_FILE}" ]; then
    grep -Fxv "${dk}" "${TRACKED_DISKS_FILE}" > "${TRACKED_DISKS_FILE}.tmp" || true
    mv "${TRACKED_DISKS_FILE}.tmp" "${TRACKED_DISKS_FILE}"
  fi
}

rufux_mount_get_dir() {
  local target="$1"
  local out mnt
  out="$("${RUFUX}" mount "${target}")"
  mnt="$(echo "${out}" | sed -n 's/^mounted .* at //p' | tail -n 1)"
  [ -n "${mnt}" ] && [ -d "${mnt}" ] || fail "rufux mount '${target}' failed (output: '${out}')"
  echo "${mnt}" >> "${TRACKED_MNTS_FILE}"
  echo "${mnt}"
}

find_ci_path() {
  python3 - "$1" "$2" <<'PY'
import os, sys
cur = sys.argv[1]
for seg in sys.argv[2].split('/'):
    if not seg:
        continue
    hit = None
    for name in os.listdir(cur):
        if name.lower() == seg.lower():
            hit = name
            break
    if hit is None:
        sys.exit(1)
    cur = os.path.join(cur, hit)
print(cur)
PY
}

echo "=== Building synthetic test ISOs (tiny.iso, linux.iso, win11.iso) ==="

# 1. tiny.iso
TINY_SRC="${TMP}/tiny_src"
mkdir -p "${TINY_SRC}"
printf 'Hello from Rufux macOS tiny ISO!\n' > "${TINY_SRC}/hello.txt"
dd if=/dev/urandom of="${TINY_SRC}/payload.bin" bs=65536 count=4 status=none
TINY_ISO="${TMP}/tiny.iso"
/usr/bin/hdiutil makehybrid -o "${TINY_ISO}" "${TINY_SRC}" -iso -joliet -default-volume-name "TINYISO" >/dev/null

# 2. linux.iso
LINUX_SRC="${TMP}/linux_src"
mkdir -p "${LINUX_SRC}/boot" "${LINUX_SRC}/EFI/BOOT" "${LINUX_SRC}/casper"
printf 'Rufux Linux Live Test README\n' > "${LINUX_SRC}/README.txt"
dd if=/dev/urandom of="${LINUX_SRC}/boot/vmlinuz" bs=65536 count=6 status=none
dd if=/dev/urandom of="${LINUX_SRC}/casper/filesystem.squashfs" bs=65536 count=12 status=none
python3 "${ROOT_DIR}/tests/mkpe.py" pe "${LINUX_SRC}/EFI/BOOT/bootx64.efi"
VMLINUZ_SHA="$(shasum -a 256 "${LINUX_SRC}/boot/vmlinuz" | awk '{print $1}')"
LINUX_ISO="${TMP}/linux.iso"
/usr/bin/hdiutil makehybrid -o "${LINUX_ISO}" "${LINUX_SRC}" -iso -joliet -default-volume-name "LINUXLIVE" >/dev/null

# 3. win11.iso (valid 2-image boot.wim with SYSTEM hive + 1.6 MiB uncompressed install.wim + PE bootx64.efi)
WIN_SRC="${TMP}/win_src"
mkdir -p "${WIN_SRC}/sources" "${WIN_SRC}/boot" "${WIN_SRC}/efi/microsoft/boot" "${WIN_SRC}/efi/boot"
printf 'MZ-BOOTMGR-STUB-PAYLOAD\n' > "${WIN_SRC}/bootmgr"
printf 'MZ-ROOT-SETUP-EXE\n' > "${WIN_SRC}/setup.exe"
printf 'MZ-SOURCES-SETUP-EXE\n' > "${WIN_SRC}/sources/setup.exe"
printf 'BCD-BIOS-STORE\n' > "${WIN_SRC}/boot/bcd"
printf 'BCD-UEFI-STORE\n' > "${WIN_SRC}/efi/microsoft/boot/bcd"
python3 "${ROOT_DIR}/tests/mkpe.py" pe "${WIN_SRC}/efi/boot/bootx64.efi"

WIM_IMG1="${TMP}/wim_img1"
WIM_IMG2="${TMP}/wim_img2"
mkdir -p "${WIM_IMG1}" "${WIM_IMG2}/Windows/System32/Config"
printf 'WinPE image 1\n' > "${WIM_IMG1}/winpe.txt"
python3 "${ROOT_DIR}/tests/mkhive.py" "${WIM_IMG2}/Windows/System32/Config/SYSTEM"
wimlib-imagex capture "${WIM_IMG1}" "${WIN_SRC}/sources/boot.wim" "Microsoft Windows PE" --compress=none >/dev/null
wimlib-imagex append "${WIM_IMG2}" "${WIN_SRC}/sources/boot.wim" "Microsoft Windows Setup" >/dev/null

INST_ROOT="${TMP}/wim_install_root"
mkdir -p "${INST_ROOT}/Windows"
dd if=/dev/urandom of="${INST_ROOT}/Windows/explorer_payload.bin" bs=65536 count=25 status=none
wimlib-imagex capture "${INST_ROOT}" "${WIN_SRC}/sources/install.wim" "Windows 11 Pro" --compress=none >/dev/null
INSTALL_WIM_SHA="$(shasum -a 256 "${WIN_SRC}/sources/install.wim" | awk '{print $1}')"

WIN11_ISO="${TMP}/win11.iso"
/usr/bin/hdiutil makehybrid -o "${WIN11_ISO}" "${WIN_SRC}" -iso -joliet -udf -default-volume-name "CCCOMA_X64FRE" >/dev/null

# ==============================================================================
# Scenario 1: CLI strict option parser, typo suggestion, dry-run default, guards
# ==============================================================================
echo "=== [1/7] CLI strict option parser & dry-run safety ==="
DISK_GUARD="${TMP}/guard.img"
printf 'SENTINEL-UNTOUCHED' > "${DISK_GUARD}"

set +e
TYPO_OUT="$("${RUFUX}" create "${TINY_ISO}" "${DISK_GUARD}" --mode dd --rela 2>&1)"
TYPO_RC=$?
set -e
[ "${TYPO_RC}" -eq 2 ] || fail "expected exit 2 on --rela typo, got ${TYPO_RC}"
echo "${TYPO_OUT}" | grep -F "unknown option '--rela'" >/dev/null || fail "missing unknown option message: ${TYPO_OUT}"
echo "${TYPO_OUT}" | grep -F "did you mean '--real'?" >/dev/null || fail "missing '--real' suggestion: ${TYPO_OUT}"
[ "$(cat "${DISK_GUARD}")" = "SENTINEL-UNTOUCHED" ] || fail "guard.img modified after rejected typo"

DRY_OUT="$("${RUFUX}" create "${TINY_ISO}" "${DISK_GUARD}" --mode dd --allow-file)"
echo "${DRY_OUT}" | grep -F "[dry-run]" >/dev/null || fail "dry-run output lacked [dry-run]: ${DRY_OUT}"
echo "${DRY_OUT}" | grep -F "Plan OK (dry-run, nothing written)." >/dev/null || fail "dry-run output lacked Plan OK: ${DRY_OUT}"
[ "$(cat "${DISK_GUARD}")" = "SENTINEL-UNTOUCHED" ] || fail "guard.img modified during dry-run"

if "${RUFUX}" create "${TINY_ISO}" "${DISK_GUARD}" --mode dd --allow-file --real >/dev/null 2>&1; then
  fail "--real without --yes must be refused"
fi
[ "$(cat "${DISK_GUARD}")" = "SENTINEL-UNTOUCHED" ] || fail "guard.img modified when --yes omitted"

if "${RUFUX}" create "${TINY_ISO}" /dev/disk0 --mode dd --allow-fixed --real --yes >/dev/null 2>&1; then
  fail "boot disk /dev/disk0 must be unconditionally refused"
fi
rm -f "${DISK_GUARD}"
echo "=== [1/7] PASS ==="

# ==============================================================================
# Scenario 2: dd mode with read-back --verify
# ==============================================================================
echo "=== [2/7] create --mode dd --verify ==="
DD_IMG="${TMP}/dd_disk.img"
TINY_LEN="$(stat -f%z "${TINY_ISO}")"
dd if=/dev/zero of="${DD_IMG}" bs=1048576 count=0 seek=8 status=none

"${RUFUX}" create "${TINY_ISO}" "${DD_IMG}" --mode dd --verify --allow-file --real --yes
TINY_SHA="$(shasum -a 256 "${TINY_ISO}" | awk '{print $1}')"
DD_HEAD_SHA="$(head -c "${TINY_LEN}" "${DD_IMG}" | shasum -a 256 | awk '{print $1}')"
[ "${DD_HEAD_SHA}" = "${TINY_SHA}" ] || fail "dd written bytes SHA-256 mismatch (${DD_HEAD_SHA} != ${TINY_SHA})"
rm -f "${DD_IMG}"
echo "=== [2/7] PASS ==="

# ==============================================================================
# Scenario 3: extract mode (GPT + vfat + --persist-mb 16 + autorun.inf)
# ==============================================================================
echo "=== [3/7] create --mode extract --scheme gpt --fs vfat --persist-mb 16 ==="
LINUX_FAT_IMG="${TMP}/linux_vfat.img"
dd if=/dev/zero of="${LINUX_FAT_IMG}" bs=1048576 count=0 seek=96 status=none

"${RUFUX}" create "${LINUX_ISO}" "${LINUX_FAT_IMG}" \
  --mode extract --scheme gpt --fs vfat --label LINUXUSB --persist-mb 16 \
  --uefi-validate --allow-file --real --yes

python3 - "${LINUX_FAT_IMG}" <<'PY'
import sys
with open(sys.argv[1], "rb") as f:
    mbr = f.read(512)
    gpt = f.read(512)
assert mbr[510:512] == b"\x55\xaa", "missing MBR 0x55AA"
assert any(b != 0 for b in mbr[:440]), "expected non-zero GPT Syslinux MBR boot code"
assert mbr[446 + 4] == 0xEE, "expected protective GPT 0xEE entry"
assert gpt[:8] == b"EFI PART", "missing EFI PART header at LBA 1"
PY

DK="$(attach_raw_rw "${LINUX_FAT_IMG}")"
MNT="$(rufux_mount_get_dir "${DK}s1")"

VM_PATH="$(find_ci_path "${MNT}" "boot/vmlinuz")" || fail "boot/vmlinuz missing on vfat extract"
[ "$(shasum -a 256 "${VM_PATH}" | awk '{print $1}')" = "${VMLINUZ_SHA}" ] || fail "vmlinuz SHA mismatch on vfat"
find_ci_path "${MNT}" "EFI/BOOT/bootx64.efi" >/dev/null || fail "EFI/BOOT/bootx64.efi missing on vfat"
AR_PATH="$(find_ci_path "${MNT}" "autorun.inf")" || fail "autorun.inf missing on vfat"
grep -F "Label=LINUXUSB" "${AR_PATH}" >/dev/null || fail "autorun.inf missing Label=LINUXUSB"
PERSIST_PATH="$(find_ci_path "${MNT}" "casper-rw")" || fail "casper-rw missing on vfat"
[ "$(stat -f%z "${PERSIST_PATH}")" -eq 16777216 ] || fail "casper-rw size != 16 MiB"
cp "${PERSIST_PATH}" "${TMP}/casper_copy.ext4"
e2fsck -fn "${TMP}/casper_copy.ext4" >/dev/null || fail "e2fsck failed on casper-rw"
rm -f "${TMP}/casper_copy.ext4"

"${RUFUX}" umount "${DK}s1"
detach_raw "${DK}"
rm -f "${LINUX_FAT_IMG}"
echo "=== [3/7] PASS ==="

# ==============================================================================
# Scenario 4: extract mode (GPT + ext4 + e2fsck + rufux mount RW NFSv3)
# ==============================================================================
echo "=== [4/7] create --mode extract --scheme gpt --fs ext4 ==="
LINUX_EXT_IMG="${TMP}/linux_ext4.img"
dd if=/dev/zero of="${LINUX_EXT_IMG}" bs=1048576 count=0 seek=64 status=none

env -u RUFUX_TMPDIR -u TMPDIR "${RUFUX}" create "${LINUX_ISO}" "${LINUX_EXT_IMG}" \
  --mode extract --scheme gpt --fs ext4 --label EXT4USB \
  --allow-file --real --yes

DK="$(attach_raw_rw "${LINUX_EXT_IMG}")"
e2fsck -fn "${DK}s1" >/dev/null || fail "e2fsck -fn failed on populated ext4 partition"
MNT="$(rufux_mount_get_dir "${DK}s1")"

VM_PATH="$(find_ci_path "${MNT}" "boot/vmlinuz")" || fail "boot/vmlinuz missing on ext4 extract"
[ "$(shasum -a 256 "${VM_PATH}" | awk '{print $1}')" = "${VMLINUZ_SHA}" ] || fail "vmlinuz SHA mismatch on ext4"
find_ci_path "${MNT}" "EFI/BOOT/bootx64.efi" >/dev/null || fail "EFI/BOOT/bootx64.efi missing on ext4"
AR_PATH="$(find_ci_path "${MNT}" "autorun.inf")" || fail "autorun.inf missing on ext4"
grep -F "Label=EXT4USB" "${AR_PATH}" >/dev/null || fail "autorun.inf missing Label=EXT4USB"

"${RUFUX}" umount "${DK}s1"
detach_raw "${DK}"
rm -f "${LINUX_EXT_IMG}"
echo "=== [4/7] PASS ==="

# ==============================================================================
# Scenario 5: dos mode (FreeDOS MBR, FAT32 PBR, KERNEL.SYS first entry, COMMAND.COM)
# ==============================================================================
echo "=== [5/7] create --mode dos --label FREEDOS ==="
DOS_IMG="${TMP}/freedos.img"
dd if=/dev/zero of="${DOS_IMG}" bs=1048576 count=0 seek=64 status=none

"${RUFUX}" create none "${DOS_IMG}" --mode dos --label FREEDOS --allow-file --real --yes

python3 - "${DOS_IMG}" <<'PY'
import struct, sys
with open(sys.argv[1], "rb") as f:
    mbr = f.read(512)
    assert mbr[510:512] == b"\x55\xaa", "missing DOS MBR 0x55AA"
    assert any(b != 0 for b in mbr[:440]), "DOS MBR boot code is all zeros"
    p1 = mbr[446:462]
    assert p1[0] == 0x80, f"expected active 0x80 flag, got 0x{p1[0]:02x}"
    assert p1[4] == 0x0C, f"expected FAT32 LBA type 0x0C, got 0x{p1[4]:02x}"
    p1_lba = struct.unpack_from("<I", p1, 8)[0]
    assert p1_lba == 2048, f"expected p1 start LBA 2048, got {p1_lba}"

    p1_off = p1_lba * 512
    f.seek(p1_off)
    pbr = f.read(1536)
    assert pbr[0:3] == b"\xeb\x58\x90", f"unexpected FAT32 PBR jump: {pbr[0:3]!r}"
    assert pbr[0x47:0x52] == b"FREEDOS    ", f"unexpected PBR label: {pbr[0x47:0x52]!r}"
    assert pbr[0x52:0x5a] == b"FAT32   ", f"unexpected PBR fstype: {pbr[0x52:0x5a]!r}"
    assert pbr[0x1fe:0x200] == b"\x55\xaa" and pbr[0x5fe:0x600] == b"\x55\xaa"

    bps = struct.unpack_from("<H", pbr, 11)[0]
    spc = pbr[13]
    rsvd = struct.unpack_from("<H", pbr, 14)[0]
    nfats = pbr[16]
    fatsz = struct.unpack_from("<I", pbr, 36)[0]
    rootclus = struct.unpack_from("<I", pbr, 44)[0]
    root_off = p1_off + (rsvd + nfats * fatsz + (rootclus - 2) * spc) * bps
    f.seek(root_off)
    rdir = f.read(4096)
    file_entries = []
    for off in range(0, len(rdir), 32):
        ent = rdir[off:off+32]
        if ent[0] == 0x00:
            break
        if ent[0] == 0xE5 or ent[11] == 0x0F or (ent[11] & 0x08):
            continue
        file_entries.append(ent[:11])
    assert len(file_entries) >= 2, f"too few root entries: {file_entries}"
    assert file_entries[0] == b"KERNEL  SYS", f"KERNEL.SYS must be first root file entry, got {file_entries[0]!r}"
    assert b"COMMAND COM" in file_entries, f"COMMAND.COM missing from root dir: {file_entries}"
PY

DK="$(attach_raw_rw "${DOS_IMG}")"
MNT="$(rufux_mount_get_dir "${DK}s1")"
cmp -s "${MNT}/KERNEL.SYS" "${ROOT_DIR}/res/freedos/KERNEL.SYS" || fail "KERNEL.SYS content mismatch"
cmp -s "${MNT}/COMMAND.COM" "${ROOT_DIR}/res/freedos/COMMAND.COM" || fail "COMMAND.COM content mismatch"
"${RUFUX}" umount "${DK}s1"
detach_raw "${DK}"
rm -f "${DOS_IMG}"
echo "=== [5/7] PASS ==="

# ==============================================================================
# Scenario 6: Windows 11 GPT + NTFS + WUE + UEFI:NTFS + GRUB2 BIOS boot
# ==============================================================================
echo "=== [6/7] create --mode windows --scheme gpt --fs ntfs --wue ... ==="
WIN_NTFS_IMG="${TMP}/win_ntfs.img"
dd if=/dev/zero of="${WIN_NTFS_IMG}" bs=1048576 count=0 seek=96 status=none

WIN_NTFS_OUT="$("${RUFUX}" create "${WIN11_ISO}" "${WIN_NTFS_IMG}" \
  --mode windows --scheme gpt --fs ntfs --label WIN11NTFS \
  --wue bypass,nro,privacy,bitlocker,locale,qol,user=Alice \
  --allow-file --real --yes)"
printf '%s\n' "${WIN_NTFS_OUT}"
echo "${WIN_NTFS_OUT}" | grep -E "Streaming .* into NTFS" >/dev/null || \
  fail "Scenario 6 did not execute direct rufux_ntfs_write_single_file WIM streaming path"

python3 - "${WIN_NTFS_IMG}" "${ROOT_DIR}/res/uefi/uefi-ntfs.img" <<'PY'
import struct, sys

GUID_MS_BASIC_DATA = bytes([0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                            0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7])
GUID_BIOS_BOOT     = bytes([0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6F, 0x6E,
                            0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49])

uefi_ref = open(sys.argv[2], "rb").read()
assert len(uefi_ref) == 1048576

with open(sys.argv[1], "rb") as f:
    mbr = f.read(512)
    gpt = f.read(512)
    assert mbr[510:512] == b"\x55\xaa", "missing protective MBR 0x55AA"
    assert mbr[446 + 4] == 0xEE, "protective 0xEE entry damaged"
    assert mbr[0] == 0xEB and mbr[0x66:0x68] == b"\x90\x90", "expected GRUB2 boot.img with 0x66 NOP patch in protective MBR"
    assert gpt[:8] == b"EFI PART", "GPT header damaged"

    ent_lba = struct.unpack_from("<Q", gpt, 72)[0]
    num_ent, ent_sz = struct.unpack_from("<II", gpt, 80)
    f.seek(ent_lba * 512)
    ents = []
    for _ in range(num_ent):
        raw = f.read(ent_sz)
        tguid = raw[:16]
        if any(b != 0 for b in tguid):
            flba, llba, attrs = struct.unpack_from("<QQQ", raw, 32)
            name = raw[56:128].decode("utf-16-le").rstrip("\x00")
            ents.append((tguid, flba, llba, attrs, name))

    assert len(ents) == 3, f"expected 3 GPT partitions, got {len(ents)}: {ents}"

    # Partition 1: Windows NTFS
    assert ents[0][0] == GUID_MS_BASIC_DATA and ents[0][4] == "Windows", f"unexpected p1: {ents[0]}"

    # Partition 2: UEFI:NTFS with bit 63 set and exact uefi-ntfs.img payload
    assert ents[1][0] == GUID_MS_BASIC_DATA and ents[1][4] == "UEFI:NTFS", f"unexpected p2: {ents[1]}"
    assert ents[1][3] == 0x8000000000000000, f"expected GPT attr bit 63 on UEFI:NTFS, got 0x{ents[1][3]:x}"
    f.seek(ents[1][1] * 512)
    p2_bytes = f.read(1048576)
    assert p2_bytes == uefi_ref, "UEFI:NTFS partition bytes do not match res/uefi/uefi-ntfs.img"

    # Partition 3: BIOS boot with GRUB2 core.img + patched diskboot.img blocklist
    assert ents[2][0] == GUID_BIOS_BOOT and ents[2][4] == "BIOS boot", f"unexpected p3: {ents[2]}"
    bios_lba = ents[2][1]
    boot_core_lba = struct.unpack_from("<Q", mbr, 0x5c)[0]
    assert boot_core_lba == bios_lba, f"MBR boot.img core_lba ({boot_core_lba}) != BIOS boot LBA ({bios_lba})"

    f.seek(bios_lba * 512)
    core0 = f.read(512)
    bl_start, bl_len, bl_seg = struct.unpack_from("<QHH", core0, 500)
    assert bl_start == bios_lba + 1, f"diskboot blocklist start ({bl_start}) != {bios_lba + 1}"
    assert bl_len > 10, f"unexpected diskboot blocklist sector count: {bl_len}"
    assert bl_seg == 0x0820, f"unexpected diskboot blocklist segment: 0x{bl_seg:04x}"
PY

DK="$(attach_raw_rw "${WIN_NTFS_IMG}")"
ntfsfix -n "${DK}s1" >/dev/null || fail "ntfsfix -n reported errors on Windows NTFS partition"
MNT="$(rufux_mount_get_dir "${DK}s1")"

UNATTEND_PATH="$(find_ci_path "${MNT}" "sources/\$OEM\$/\$\$/Panther/unattend.xml")" || \
  fail "sources/\$OEM\$/\$\$/Panther/unattend.xml missing on NTFS stick"
grep -F "<Name>Alice</Name>" "${UNATTEND_PATH}" >/dev/null || fail "unattend.xml missing <Name>Alice</Name>"
grep -F "BypassNRO" "${UNATTEND_PATH}" >/dev/null || fail "unattend.xml missing BypassNRO"
grep -F "<ProtectYourPC>3</ProtectYourPC>" "${UNATTEND_PATH}" >/dev/null || fail "unattend.xml missing ProtectYourPC=3"
grep -F "<PreventDeviceEncryption>true</PreventDeviceEncryption>" "${UNATTEND_PATH}" >/dev/null || fail "unattend.xml missing BitLocker bypass"
grep -F "ShowCopilotButton" "${UNATTEND_PATH}" >/dev/null || fail "unattend.xml missing QoL registry tweaks"
grep -F "<InputLocale>" "${UNATTEND_PATH}" >/dev/null || fail "unattend.xml missing InputLocale from --wue locale"
grep -F "<TimeZone>" "${UNATTEND_PATH}" >/dev/null || fail "unattend.xml missing TimeZone from --wue locale"

BOOTWIM_PATH="$(find_ci_path "${MNT}" "sources/boot.wim")" || fail "sources/boot.wim missing on NTFS stick"
HIVE_CHK="${TMP}/hive_chk_ntfs"
mkdir -p "${HIVE_CHK}"
WIMLIB_IMAGEX_IGNORE_CASE=1 wimlib-imagex extract "${BOOTWIM_PATH}" 2 "/Windows/System32/config/SYSTEM" "--dest-dir=${HIVE_CHK}" --no-acls >/dev/null
printf 'cd \\Setup\\LabConfig\nlsval\n' > "${HIVE_CHK}/verify.hsh"
HIVE_OUT="$(hivexsh -f "${HIVE_CHK}/verify.hsh" "${HIVE_CHK}/SYSTEM")"
echo "${HIVE_OUT}" | grep -F '"BypassTPMCheck"=dword:00000001' >/dev/null || fail "boot.wim lacks BypassTPMCheck=1: ${HIVE_OUT}"
echo "${HIVE_OUT}" | grep -F '"BypassSecureBootCheck"=dword:00000001' >/dev/null || fail "boot.wim lacks BypassSecureBootCheck=1: ${HIVE_OUT}"
echo "${HIVE_OUT}" | grep -F '"BypassRAMCheck"=dword:00000001' >/dev/null || fail "boot.wim lacks BypassRAMCheck=1: ${HIVE_OUT}"

INSTWIM_PATH="$(find_ci_path "${MNT}" "sources/install.wim")" || fail "sources/install.wim missing on NTFS stick"
[ "$(shasum -a 256 "${INSTWIM_PATH}" | awk '{print $1}')" = "${INSTALL_WIM_SHA}" ] || \
  fail "sources/install.wim SHA-256 mismatch on NTFS stick"

"${RUFUX}" umount "${DK}s1"
detach_raw "${DK}"
rm -rf "${WIN_NTFS_IMG}" "${HIVE_CHK}"
echo "=== [6/7] PASS ==="

# ==============================================================================
# Scenario 7: Windows 11 MBR + FAT32 + WIM split (--split-wim 1) + GRUB2 MBR gap
# ==============================================================================
echo "=== [7/7] create --mode windows --scheme mbr --fs vfat --split-wim 1 ==="
WIN_FAT_IMG="${TMP}/win_fat.img"
dd if=/dev/zero of="${WIN_FAT_IMG}" bs=1048576 count=0 seek=96 status=none

"${RUFUX}" create "${WIN11_ISO}" "${WIN_FAT_IMG}" \
  --mode windows --scheme mbr --fs vfat --label WIN11FAT \
  --split-wim 1 --wue bypass \
  --allow-file --real --yes

python3 - "${WIN_FAT_IMG}" <<'PY'
import struct, sys
with open(sys.argv[1], "rb") as f:
    mbr = f.read(512)
    assert mbr[510:512] == b"\x55\xaa", "missing MBR 0x55AA"
    assert mbr[0] == 0xEB and mbr[0x66:0x68] == b"\x90\x90", "expected GRUB2 boot.img with 0x66 NOP patch in MBR"
    boot_core_lba = struct.unpack_from("<Q", mbr, 0x5c)[0]
    assert boot_core_lba == 1, f"expected MBR gap core_lba == 1, got {boot_core_lba}"

    p1 = mbr[446:462]
    assert p1[0] == 0x80 and p1[4] == 0x0C, f"expected active FAT32 LBA p1, got status=0x{p1[0]:02x} type=0x{p1[4]:02x}"
    p1_lba = struct.unpack_from("<I", p1, 8)[0]
    assert p1_lba == 2048, f"expected p1 start LBA 2048, got {p1_lba}"

    # LBA 1 holds GRUB2 core.img sector 0 (diskboot.img); LBA 2+ holds compressed core payload
    core0 = f.read(512)
    core1 = f.read(512)
    bl_start, bl_len, bl_seg = struct.unpack_from("<QHH", core0, 500)
    assert bl_start == 2, f"expected MBR gap diskboot blocklist start == 2, got {bl_start}"
    assert 10 < bl_len < (p1_lba - 2), f"unexpected MBR gap core_sectors-1: {bl_len}"
    assert bl_seg == 0x0820, f"unexpected diskboot segment: 0x{bl_seg:04x}"
    assert any(b != 0 for b in core1), "expected non-zero GRUB2 core.img payload at LBA 2"
PY

DK="$(attach_raw_rw "${WIN_FAT_IMG}")"
MNT="$(rufux_mount_get_dir "${DK}s1")"

SWM1="$(find_ci_path "${MNT}" "sources/install.swm")" || fail "sources/install.swm missing on FAT32 split stick"
SWM2="$(find_ci_path "${MNT}" "sources/install2.swm")" || fail "sources/install2.swm missing on FAT32 split stick"
[ -s "${SWM1}" ] && [ -s "${SWM2}" ] || fail "split SWM parts are empty"
if find_ci_path "${MNT}" "sources/install.wim" >/dev/null 2>&1; then
  fail "unsplit sources/install.wim should not exist when split into .swm parts"
fi

BOOTWIM_PATH="$(find_ci_path "${MNT}" "sources/boot.wim")" || fail "sources/boot.wim missing on FAT32 stick"
HIVE_CHK2="${TMP}/hive_chk_fat"
mkdir -p "${HIVE_CHK2}"
WIMLIB_IMAGEX_IGNORE_CASE=1 wimlib-imagex extract "${BOOTWIM_PATH}" 2 "/Windows/System32/config/SYSTEM" "--dest-dir=${HIVE_CHK2}" --no-acls >/dev/null
printf 'cd \\Setup\\LabConfig\nlsval\n' > "${HIVE_CHK2}/verify.hsh"
HIVE_OUT2="$(hivexsh -f "${HIVE_CHK2}/verify.hsh" "${HIVE_CHK2}/SYSTEM")"
echo "${HIVE_OUT2}" | grep -F '"BypassTPMCheck"=dword:00000001' >/dev/null || fail "FAT32 boot.wim lacks BypassTPMCheck=1"

"${RUFUX}" umount "${DK}s1"
detach_raw "${DK}"
rm -rf "${WIN_FAT_IMG}" "${HIVE_CHK2}"

# Regression guard: rufux_windows_bios_install must reject empty MBR and LBA-1 MBR
# without writing over LBA 1 (partition 1 start).
LIBCORE="$(dirname "${RUFUX}")/librufux_core.a"
if [ -f "${LIBCORE}" ]; then
  CHK_BIOS_SRC="${TMP}/chk_biosboot_gap.c"
  CHK_BIOS_BIN="${TMP}/chk_biosboot_gap"
  cat > "${CHK_BIOS_SRC}" <<'EOF'
#include "biosboot.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int write_img(const char *path, int with_lba1_part, unsigned char sentinel) {
  unsigned char buf[65536];
  memset(buf, 0, sizeof buf);
  buf[510] = 0x55;
  buf[511] = 0xAA;
  if (with_lba1_part) {
    buf[446 + 0] = 0x80;
    buf[446 + 4] = 0x0C;
    buf[446 + 8] = 1;    // start LBA = 1 (zero MBR gap)
    buf[446 + 12] = 100; // sector count = 100
  }
  memset(buf + 512, sentinel, 512);
  int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (fd < 0) return -1;
  ssize_t w = write(fd, buf, sizeof buf);
  close(fd);
  return w == (ssize_t)sizeof buf ? 0 : -1;
}

static int check_sentinel(const char *path, unsigned char sentinel) {
  unsigned char lba1[512];
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  ssize_t r = pread(fd, lba1, sizeof lba1, 512);
  close(fd);
  if (r != (ssize_t)sizeof lba1) return -1;
  for (size_t i = 0; i < sizeof lba1; i++) {
    if (lba1[i] != sentinel) return -1;
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 3) return 2;
  const char *empty_img = argv[1];
  const char *lba1_img = argv[2];
  char err[512] = {0};

  if (write_img(empty_img, 0, 0xA5) != 0) return 3;
  if (rufux_windows_bios_install(empty_img, NULL, NULL, NULL, err, sizeof err) == 0) {
    fprintf(stderr, "unexpected success on empty MBR\n");
    return 4;
  }
  if (!strstr(err, "no MBR partitions") || check_sentinel(empty_img, 0xA5) != 0) {
    fprintf(stderr, "empty MBR check failed: err='%s'\n", err);
    return 5;
  }

  err[0] = 0;
  if (write_img(lba1_img, 1, 0x5A) != 0) return 6;
  if (rufux_windows_bios_install(lba1_img, NULL, NULL, NULL, err, sizeof err) == 0) {
    fprintf(stderr, "unexpected success on LBA-1 MBR\n");
    return 7;
  }
  if (!strstr(err, "MBR gap before partition 1") || check_sentinel(lba1_img, 0x5A) != 0) {
    fprintf(stderr, "LBA-1 MBR check failed: err='%s'\n", err);
    return 8;
  }
  return 0;
}
EOF
  cc -Wall -Wextra -I"${MACOS_DIR}/src" "${CHK_BIOS_SRC}" "${LIBCORE}" \
    -L"${MACOS_DIR}/vendor/ntfs-3g/lib" -Wl,-rpath,"${MACOS_DIR}/vendor/ntfs-3g/lib" -lntfs-3g \
    -framework IOKit -framework CoreFoundation -framework DiskArbitration \
    -o "${CHK_BIOS_BIN}" || fail "failed to compile biosboot MBR-gap regression harness"
  "${CHK_BIOS_BIN}" "${TMP}/empty_mbr.img" "${TMP}/lba1_mbr.img" || \
    fail "biosboot MBR-gap regression check failed"
  rm -f "${CHK_BIOS_SRC}" "${CHK_BIOS_BIN}" "${TMP}/empty_mbr.img" "${TMP}/lba1_mbr.img"
fi

echo "=== [7/7] PASS ==="

echo "test_create_e2e.sh: ALL 7 END-TO-END CREATE & CLI SCENARIOS PASSED"

#!/usr/bin/env bash
set -euo pipefail

MACOS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT_DIR="$(cd "${MACOS_DIR}/.." && pwd)"
APP_DIR="${1:-${MACOS_DIR}/dist/Rufux.app}"
DMG_PATH="${2:-${MACOS_DIR}/dist/Rufux-2.3.0-macos-arm64.dmg}"

TMP="$(mktemp -d /tmp/rufux-test-bundle-dmg-XXXXXX)"
CLEAN_ROOT="/tmp/Rufux Clean App Test"
CLEAN_APP="${CLEAN_ROOT}/Rufux.app"
MNT_ROOT="${TMP}/mnt_root"
STATE_DIR="${TMP}/mnt_state"
TMP_SCRATCH="${TMP}/scratch"
TMP_HOME="${TMP}/home"
TMP_SYS="${TMP}/tmp"
DMG_MNT="${TMP}/dmg_mnt"
mkdir -p "${MNT_ROOT}" "${STATE_DIR}" "${TMP_SCRATCH}" "${TMP_HOME}" "${TMP_SYS}" "${DMG_MNT}"

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
        if [ -x "${CLEAN_APP}/Contents/MacOS/rufux" ]; then
          env -i HOME="${TMP_HOME}" TMPDIR="${TMP_SYS}" PATH="/usr/bin:/bin:/usr/sbin:/sbin" \
            RUFUX_MOUNT_ROOT="${MNT_ROOT}" RUFUX_MOUNT_STATE_DIR="${STATE_DIR}" \
            "${CLEAN_APP}/Contents/MacOS/rufux" umount "${m}" >/dev/null 2>&1 || true
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
  rm -rf "${CLEAN_ROOT}" "${TMP}"
  exit "${rc}"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

run_clean() {
  env -i \
    HOME="${TMP_HOME}" \
    TMPDIR="${TMP_SYS}" \
    RUFUX_TMPDIR="${TMP_SCRATCH}" \
    PATH="/usr/bin:/bin:/usr/sbin:/sbin" \
    RUFUX_MOUNT_ROOT="${MNT_ROOT}" \
    RUFUX_MOUNT_STATE_DIR="${STATE_DIR}" \
    RUFUX_NO_GUI_HANDOFF=1 \
    "$@"
}

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

clean_mount_get_dir() {
  local target="$1"
  local out mnt
  out="$(run_clean "${CLEAN_APP}/Contents/MacOS/rufux" mount "${target}")"
  mnt="$(echo "${out}" | sed -n 's/^mounted .* at //p' | tail -n 1)"
  [ -n "${mnt}" ] && [ -d "${mnt}" ] || fail "clean-room rufux mount '${target}' failed (output: '${out}')"
  echo "${mnt}" >> "${TRACKED_MNTS_FILE}"
  echo "${mnt}"
}

find_ci_path() {
  /usr/bin/python3 - "$1" "$2" <<'PY'
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

test_bundle_macho_self_contained_and_clean_env() {
  echo "=== [1/5] Checking Rufux.app and DMG existence & structure ==="
  [ -d "${APP_DIR}" ] || fail "Rufux.app bundle does not exist at ${APP_DIR}"
  [ -f "${DMG_PATH}" ] && [ -s "${DMG_PATH}" ] || fail "DMG does not exist or is empty at ${DMG_PATH}"

  [ -f "${APP_DIR}/Contents/Info.plist" ] || fail "Missing Contents/Info.plist"
  [ -f "${APP_DIR}/Contents/Resources/Rufux.icns" ] && [ -s "${APP_DIR}/Contents/Resources/Rufux.icns" ] || \
    fail "Missing Contents/Resources/Rufux.icns"
  [ -x "${APP_DIR}/Contents/MacOS/rufux-gui" ] || fail "Missing executable Contents/MacOS/rufux-gui"
  [ -x "${APP_DIR}/Contents/MacOS/rufux" ] || fail "Missing executable Contents/MacOS/rufux"

  for tool in rufux rufux-mountd mkntfs ntfsfix mkfs.ntfs mke2fs e2fsck mkfs.ext2 mkfs.ext3 mkfs.ext4 \
              7zz 7z wimlib-imagex hivexsh smartctl qemu-img i686-elf-grub-mkimage grub-mkimage; do
    [ -x "${APP_DIR}/Contents/Resources/bin/${tool}" ] || \
      fail "Missing bundled tool Contents/Resources/bin/${tool}"
  done

  [ -f "${APP_DIR}/Contents/Resources/share/rufux/freedos/KERNEL.SYS" ] || \
    fail "Missing bundled FreeDOS KERNEL.SYS"
  [ -f "${APP_DIR}/Contents/Resources/share/rufux/uefi/uefi-ntfs.img" ] || \
    fail "Missing bundled uefi/uefi-ntfs.img"
  [ -f "${APP_DIR}/Contents/Resources/share/rufux/dbx/dbx_x64.efiauth2" ] || \
    fail "Missing bundled dbx/dbx_x64.efiauth2"

  echo "=== [2/5] Inspecting Mach-O load commands & LC_RPATHs across Rufux.app ==="
  local macho_count=0
  while IFS= read -r -d '' f; do
    if ! /usr/bin/file -b "${f}" | grep -q "Mach-O"; then
      continue
    fi
    macho_count=$((macho_count + 1))
    local deps
    deps="$(/usr/bin/otool -L "${f}" | tail -n +2 | awk '{print $1}')"
    while IFS= read -r dep; do
      [ -n "${dep}" ] || continue
      case "${dep}" in
        /System/Library/*|/usr/lib/*|@executable_path/*|@loader_path/*|@rpath/*)
          ;;
        *)
          fail "Non-self-contained Mach-O dependency '${dep}' in '${f}'"
          ;;
      esac
      case "${dep}" in
        *homebrew*|*/usr/local/*|*/Users/*|*/vendor/*)
          fail "Leaked host path '${dep}' in '${f}'"
          ;;
      esac
    done <<< "${deps}"

    local rpaths
    rpaths="$(/usr/bin/otool -l "${f}" | awk '/cmd LC_RPATH/{getline; getline; print $2}')"
    if [ -n "${rpaths}" ]; then
      while IFS= read -r rp; do
        [ -n "${rp}" ] || continue
        case "${rp}" in
          *homebrew*|*/usr/local/*|*/Users/*|*/vendor/*)
            fail "Leaked build-host LC_RPATH '${rp}' in '${f}'"
            ;;
        esac
      done <<< "${rpaths}"
    fi
  done < <(find "${APP_DIR}" -type f -print0)
  [ "${macho_count}" -ge 12 ] || fail "Expected at least 12 Mach-O files in Rufux.app, found ${macho_count}"
  echo "Verified ${macho_count} Mach-O binaries/dylibs are 100% self-contained."

  echo "=== [3/5] Verifying ad-hoc code signature on Rufux.app ==="
  /usr/bin/codesign --verify --deep --strict --verbose=2 "${APP_DIR}" 2>&1
  local cs_info
  cs_info="$(/usr/bin/codesign -dv "${APP_DIR}" 2>&1)"
  echo "${cs_info}" | grep -F "Signature=adhoc" >/dev/null || \
    fail "Expected Signature=adhoc on Rufux.app, got: ${cs_info}"

  echo "=== [4/5] Running clean-room execution test from path with spaces ==="
  rm -rf "${CLEAN_ROOT}"
  mkdir -p "${CLEAN_ROOT}"
  /usr/bin/ditto "${APP_DIR}" "${CLEAN_APP}"

  # 4a. list --json in clean room
  local list_json
  list_json="$(run_clean "${CLEAN_APP}/Contents/MacOS/rufux" list --json --allow-fixed)"
  /usr/bin/python3 -c '
import json, sys
arr = json.loads(sys.argv[1])
assert isinstance(arr, list), "expected JSON list"
for d in arr:
    assert d.get("node") != "/dev/disk0", "boot disk /dev/disk0 must never appear in list"
' "${list_json}"

  # 4b. Partition, format, populate, and RW-mount/unmount both NTFS and ext4 in clean room
  local tiny_src="${TMP}/tiny_src"
  mkdir -p "${tiny_src}/sub"
  printf 'Clean-room payload line\n' > "${tiny_src}/sub/clean.txt"
  dd if=/dev/urandom of="${tiny_src}/sub/random.bin" bs=65536 count=8 status=none
  local want_sha
  want_sha="$(/usr/bin/shasum -a 256 "${tiny_src}/sub/random.bin" | awk '{print $1}')"
  local tiny_iso="${TMP}/tiny.iso"
  /usr/bin/hdiutil makehybrid -o "${tiny_iso}" "${tiny_src}" -iso -joliet -default-volume-name "CLEANISO" >/dev/null

  for fstype in ntfs ext4; do
    local fs_img="${TMP}/clean_${fstype}.img"
    dd if=/dev/zero of="${fs_img}" bs=1048576 count=0 seek=64 status=none
    local label="CLN_${fstype}"
    run_clean "${CLEAN_APP}/Contents/MacOS/rufux" create "${tiny_iso}" "${fs_img}" \
      --mode extract --scheme gpt --fs "${fstype}" --label "${label}" \
      --allow-file --real --yes

    local dk mnt
    dk="$(attach_raw_rw "${fs_img}")"
    mnt="$(clean_mount_get_dir "${dk}s1")"

    local rnd_path
    rnd_path="$(find_ci_path "${mnt}" "sub/random.bin")" || fail "${fstype}: sub/random.bin missing"
    [ "$(/usr/bin/shasum -a 256 "${rnd_path}" | awk '{print $1}')" = "${want_sha}" ] || \
      fail "${fstype}: random.bin SHA-256 mismatch"

    printf 'written via clean-room NFSv3 %s\n' "${fstype}" > "${mnt}/sub/rw_test.txt"
    run_clean "${CLEAN_APP}/Contents/MacOS/rufux" umount "${dk}s1"

    local mnt2
    mnt2="$(clean_mount_get_dir "${dk}s1")"
    [ "$(cat "${mnt2}/sub/rw_test.txt")" = "written via clean-room NFSv3 ${fstype}" ] || \
      fail "${fstype}: RW write did not persist across clean-room remount"
    run_clean "${CLEAN_APP}/Contents/MacOS/rufux" umount "${dk}s1"

    if [ "${fstype}" = "ntfs" ]; then
      run_clean "${CLEAN_APP}/Contents/Resources/bin/ntfsfix" -n "${dk}s1" >/dev/null
    else
      run_clean "${CLEAN_APP}/Contents/Resources/bin/e2fsck" -fn "${dk}s1" >/dev/null
    fi
    detach_raw "${dk}"
    rm -f "${fs_img}"
  done

  # 4c. Full Windows GPT+NTFS+GRUB2+WUE create in clean room
  local win_src="${TMP}/win_src"
  mkdir -p "${win_src}/sources" "${win_src}/boot" "${win_src}/efi/microsoft/boot" "${win_src}/efi/boot"
  printf 'MZ-BOOTMGR\n' > "${win_src}/bootmgr"
  printf 'MZ-SETUP\n' > "${win_src}/setup.exe"
  printf 'MZ-SRC-SETUP\n' > "${win_src}/sources/setup.exe"
  printf 'BCD-BIOS\n' > "${win_src}/boot/bcd"
  printf 'BCD-UEFI\n' > "${win_src}/efi/microsoft/boot/bcd"
  /usr/bin/python3 "${ROOT_DIR}/tests/mkpe.py" pe "${win_src}/efi/boot/bootx64.efi"

  local wim1="${TMP}/wim1" wim2="${TMP}/wim2" inst_root="${TMP}/inst_root"
  mkdir -p "${wim1}" "${wim2}/Windows/System32/Config" "${inst_root}/Windows"
  printf 'WinPE 1\n' > "${wim1}/pe.txt"
  /usr/bin/python3 "${ROOT_DIR}/tests/mkhive.py" "${wim2}/Windows/System32/Config/SYSTEM"
  run_clean "${CLEAN_APP}/Contents/Resources/bin/wimlib-imagex" capture "${wim1}" "${win_src}/sources/boot.wim" "WinPE" --compress=none >/dev/null
  run_clean "${CLEAN_APP}/Contents/Resources/bin/wimlib-imagex" append "${wim2}" "${win_src}/sources/boot.wim" "WinSetup" >/dev/null
  dd if=/dev/urandom of="${inst_root}/Windows/payload.bin" bs=65536 count=16 status=none
  run_clean "${CLEAN_APP}/Contents/Resources/bin/wimlib-imagex" capture "${inst_root}" "${win_src}/sources/install.wim" "Windows 11" --compress=none >/dev/null
  local inst_sha
  inst_sha="$(/usr/bin/shasum -a 256 "${win_src}/sources/install.wim" | awk '{print $1}')"

  local win_iso="${TMP}/win11.iso"
  /usr/bin/hdiutil makehybrid -o "${win_iso}" "${win_src}" -iso -joliet -udf -default-volume-name "WIN11CLEAN" >/dev/null

  local win_img="${TMP}/win_clean.img"
  dd if=/dev/zero of="${win_img}" bs=1048576 count=0 seek=96 status=none
  run_clean "${CLEAN_APP}/Contents/MacOS/rufux" create "${win_iso}" "${win_img}" \
    --mode windows --scheme gpt --fs ntfs --label WIN11CLN \
    --wue bypass,nro,privacy,user=CleanUser \
    --allow-file --real --yes

  /usr/bin/python3 - "${win_img}" "${CLEAN_APP}/Contents/Resources/share/rufux/uefi/uefi-ntfs.img" <<'PY'
import struct, sys
uefi_ref = open(sys.argv[2], "rb").read()
with open(sys.argv[1], "rb") as f:
    mbr = f.read(512)
    gpt = f.read(512)
    assert mbr[510:512] == b"\x55\xaa" and mbr[0] == 0xEB and mbr[0x66:0x68] == b"\x90\x90"
    assert gpt[:8] == b"EFI PART"
    ent_lba = struct.unpack_from("<Q", gpt, 72)[0]
    num_ent, ent_sz = struct.unpack_from("<II", gpt, 80)
    f.seek(ent_lba * 512)
    ents = []
    for _ in range(num_ent):
        raw = f.read(ent_sz)
        if any(b != 0 for b in raw[:16]):
            flba, llba, attrs = struct.unpack_from("<QQQ", raw, 32)
            name = raw[56:128].decode("utf-16-le").rstrip("\x00")
            ents.append((flba, llba, attrs, name))
    assert len(ents) == 3, f"expected 3 GPT partitions, got {ents}"
    f.seek(ents[1][0] * 512)
    assert f.read(len(uefi_ref)) == uefi_ref, "UEFI:NTFS payload mismatch"
    bios_lba = ents[2][0]
    assert struct.unpack_from("<Q", mbr, 0x5c)[0] == bios_lba
PY

  local wdk wmnt
  wdk="$(attach_raw_rw "${win_img}")"
  wmnt="$(clean_mount_get_dir "${wdk}s1")"
  local unattend_p bootwim_p instwim_p
  unattend_p="$(find_ci_path "${wmnt}" "sources/\$OEM\$/\$\$/Panther/unattend.xml")" || fail "unattend.xml missing"
  grep -F "<Name>CleanUser</Name>" "${unattend_p}" >/dev/null || fail "unattend.xml missing CleanUser"
  bootwim_p="$(find_ci_path "${wmnt}" "sources/boot.wim")" || fail "boot.wim missing"
  instwim_p="$(find_ci_path "${wmnt}" "sources/install.wim")" || fail "install.wim missing"
  [ "$(/usr/bin/shasum -a 256 "${instwim_p}" | awk '{print $1}')" = "${inst_sha}" ] || fail "install.wim SHA mismatch"

  local hive_chk="${TMP}/hive_chk"
  mkdir -p "${hive_chk}"
  run_clean env WIMLIB_IMAGEX_IGNORE_CASE=1 "${CLEAN_APP}/Contents/Resources/bin/wimlib-imagex" \
    extract "${bootwim_p}" 2 "/Windows/System32/config/SYSTEM" "--dest-dir=${hive_chk}" --no-acls >/dev/null
  printf 'cd \\Setup\\LabConfig\nlsval\n' > "${hive_chk}/check.hsh"
  local hive_out
  hive_out="$(run_clean "${CLEAN_APP}/Contents/Resources/bin/hivexsh" -f "${hive_chk}/check.hsh" "${hive_chk}/SYSTEM")"
  echo "${hive_out}" | grep -F '"BypassTPMCheck"=dword:00000001' >/dev/null || fail "LabConfig BypassTPMCheck missing"

  run_clean "${CLEAN_APP}/Contents/MacOS/rufux" umount "${wdk}s1"
  detach_raw "${wdk}"
  rm -rf "${win_img}" "${hive_chk}"

  echo "=== [5/5] Verifying DMG checksum, signature, and mounted contents ==="
  /usr/bin/hdiutil verify "${DMG_PATH}" >/dev/null
  /usr/bin/codesign --verify --verbose=2 "${DMG_PATH}" 2>&1
  local dmg_attach_out dmg_dev
  dmg_attach_out="$(/usr/bin/hdiutil attach -readonly -nobrowse -mountpoint "${DMG_MNT}" "${DMG_PATH}")"
  dmg_dev="$(echo "${dmg_attach_out}" | awk '/^\/dev\/disk[0-9]+/{print $1; exit}')"
  [ -n "${dmg_dev}" ] || fail "Failed to attach DMG ${DMG_PATH}"
  echo "${dmg_dev}" >> "${TRACKED_DISKS_FILE}"

  [ -d "${DMG_MNT}/Rufux.app" ] || fail "Mounted DMG is missing Rufux.app"
  [ -L "${DMG_MNT}/Applications" ] || fail "Mounted DMG is missing Applications symlink"
  [ "$(readlink "${DMG_MNT}/Applications")" = "/Applications" ] || fail "Applications symlink does not point to /Applications"
  /usr/bin/codesign --verify --deep --strict --verbose=2 "${DMG_MNT}/Rufux.app" 2>&1

  detach_raw "${dmg_dev}"
  echo "=== ALL 5 BUNDLE & DMG VERIFICATION CHECKS PASSED ==="
}

test_bundle_macho_self_contained_and_clean_env

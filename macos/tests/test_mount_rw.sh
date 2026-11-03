#!/usr/bin/env bash
set -euo pipefail

MACOS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLI="${1:-${MACOS_DIR}/build/rufux-mount-test-cli}"
MOUNTD_BIN="${MACOS_DIR}/build/rufux-mountd"

export PATH="${MACOS_DIR}/vendor/ntfs-3g/sbin:${MACOS_DIR}/vendor/ntfs-3g/bin:/opt/homebrew/opt/e2fsprogs/sbin:/opt/homebrew/opt/e2fsprogs/bin:/opt/homebrew/bin:/usr/sbin:/sbin:/usr/bin:/bin:${PATH}"

TMP_DIR="$(mktemp -d /tmp/rufux-test-mount-rw-XXXXXX)"
SPACE_DIR="${TMP_DIR}/dir with spaces"
MNT_ROOT="${TMP_DIR}/mnt root with spaces"
STATE_DIR="${TMP_DIR}/state"
mkdir -p "${SPACE_DIR}" "${MNT_ROOT}" "${STATE_DIR}"

export RUFUX_MOUNT_ROOT="${MNT_ROOT}"
export RUFUX_MOUNT_STATE_DIR="${STATE_DIR}"

ACTIVE_MNTS=()
ATTACHED_DISKS=()

cleanup() {
  set +e
  if [ "${#ACTIVE_MNTS[@]}" -gt 0 ]; then
    for m in "${ACTIVE_MNTS[@]}"; do
      if [ -n "${m}" ]; then
        if [ -x "${CLI}" ]; then
          "${CLI}" umount "${m}" >/dev/null 2>&1 || true
        fi
        /sbin/umount -f "${m}" >/dev/null 2>&1 || true
        /usr/sbin/diskutil unmount force "${m}" >/dev/null 2>&1 || true
      fi
    done
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
  if [ "${#ATTACHED_DISKS[@]}" -gt 0 ]; then
    for dk in "${ATTACHED_DISKS[@]}"; do
      if [ -n "${dk}" ]; then
        /usr/bin/hdiutil detach -force "${dk}" >/dev/null 2>&1 || true
      fi
    done
  fi
  rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

if [ ! -x "${CLI}" ]; then
  fail "rufux-mount-test-cli not built at ${CLI}"
fi
if [ ! -x "${MOUNTD_BIN}" ]; then
  fail "rufux-mountd not built at ${MOUNTD_BIN}"
fi

# 0. Verify rufux_part1 and boot-disk armor
P1_OUT="$("${CLI}" part1 /dev/disk4)"
[ "${P1_OUT}" = "/dev/disk4s1" ] || fail "expected /dev/disk4s1 from part1 /dev/disk4, got '${P1_OUT}'"
P1_RAW_OUT="$("${CLI}" part1 /dev/rdisk7)"
[ "${P1_RAW_OUT}" = "/dev/disk7s1" ] || fail "expected /dev/disk7s1 from part1 /dev/rdisk7, got '${P1_RAW_OUT}'"

if "${CLI}" mount /dev/disk0 >/dev/null 2>&1; then
  fail "rufux_mount must refuse boot disk /dev/disk0"
fi

# Generate 2 MiB random reference payload once
RAND_SRC="${TMP_DIR}/random_2m.bin"
dd if=/dev/urandom of="${RAND_SRC}" bs=65536 count=32 status=none
WANT_SHA="$(shasum -a 256 "${RAND_SRC}" | awk '{print $1}')"

run_rw_persistence_test() {
  local fstype="$1"
  local img="${SPACE_DIR}/${fstype}.img"
  local label="$2"

  echo "=== Testing ${fstype} read-write localhost NFSv3 mount (${img}) ==="
  dd if=/dev/zero of="${img}" bs=1048576 count=0 seek=64 status=none

  if [ "${fstype}" = "ntfs" ]; then
    mkntfs -F -f -q -L "${label}" "${img}" >/dev/null
  else
    mke2fs -q -t "${fstype}" -F -L "${label}" "${img}" >/dev/null
  fi

  # 1. Mount image inside path with spaces
  local mnt
  mnt="$("${CLI}" mount "${img}")"
  [ -n "${mnt}" ] || fail "${fstype}: empty mountpoint returned"
  [ -d "${mnt}" ] || fail "${fstype}: mountpoint '${mnt}' is not a directory"
  ACTIVE_MNTS+=("${mnt}")

  case "${mnt}" in
    "${MNT_ROOT}/"*) ;;
    *) fail "${fstype}: expected mountpoint under RUFUX_MOUNT_ROOT ('${MNT_ROOT}'), got '${mnt}'" ;;
  esac

  # 2. Exercise POSIX VFS syscalls: mkdir, rmdir, create, overwrite/truncate, 2 MiB dd, symlink, rename, delete, ls -la
  mkdir -p "${mnt}/subdir/ephemeral_dir"
  [ -d "${mnt}/subdir/ephemeral_dir" ] || fail "${fstype}: mkdir ephemeral_dir failed"
  rmdir "${mnt}/subdir/ephemeral_dir"
  [ ! -e "${mnt}/subdir/ephemeral_dir" ] || fail "${fstype}: rmdir ephemeral_dir failed"

  printf 'initial longer payload before truncation\n' > "${mnt}/subdir/hello world.txt"
  printf 'Hello from Rufux macOS %s RW mount!\n' "${fstype}" > "${mnt}/subdir/hello world.txt"
  local got_txt
  got_txt="$(cat "${mnt}/subdir/hello world.txt")"
  [ "${got_txt}" = "Hello from Rufux macOS ${fstype} RW mount!" ] || fail "${fstype}: text mismatch: '${got_txt}'"

  # 2 MiB binary blob write via dd + SHA-256 check
  dd if="${RAND_SRC}" of="${mnt}/subdir/blob_orig.bin" bs=65536 count=32 status=none
  local sha1
  sha1="$(shasum -a 256 "${mnt}/subdir/blob_orig.bin" | awk '{print $1}')"
  [ "${sha1}" = "${WANT_SHA}" ] || fail "${fstype}: 2 MiB blob SHA-256 mismatch before rename (${sha1} != ${WANT_SHA})"

  # Create multi-block temp file and delete it (exercises block & inode bitmap release)
  dd if="${RAND_SRC}" of="${mnt}/subdir/temp_to_delete.bin" bs=65536 count=8 status=none
  [ -f "${mnt}/subdir/temp_to_delete.bin" ] || fail "${fstype}: temp_to_delete.bin missing"
  rm -f "${mnt}/subdir/temp_to_delete.bin"
  [ ! -e "${mnt}/subdir/temp_to_delete.bin" ] || fail "${fstype}: temp_to_delete.bin still exists after rm"

  # Rename 2 MiB blob (including overwrite-rename onto an existing file)
  printf 'victim to overwrite on rename\n' > "${mnt}/subdir/blob_final.bin"
  mv -f "${mnt}/subdir/blob_orig.bin" "${mnt}/subdir/blob_final.bin"
  [ ! -e "${mnt}/subdir/blob_orig.bin" ] || fail "${fstype}: blob_orig.bin still exists after mv"
  [ -f "${mnt}/subdir/blob_final.bin" ] || fail "${fstype}: blob_final.bin missing after mv"

  local sha_after_mv
  sha_after_mv="$(shasum -a 256 "${mnt}/subdir/blob_final.bin" | awk '{print $1}')"
  [ "${sha_after_mv}" = "${WANT_SHA}" ] || fail "${fstype}: SHA-256 mismatch after rename (${sha_after_mv} != ${WANT_SHA})"

  # Symlink creation and readlink
  ln -s "hello world.txt" "${mnt}/subdir/hello_symlink.txt"
  [ -L "${mnt}/subdir/hello_symlink.txt" ] || fail "${fstype}: hello_symlink.txt is not a symlink"
  local link_target
  link_target="$(readlink "${mnt}/subdir/hello_symlink.txt")"
  [ "${link_target}" = "hello world.txt" ] || fail "${fstype}: readlink mismatch: '${link_target}'"
  local via_link
  via_link="$(cat "${mnt}/subdir/hello_symlink.txt")"
  [ "${via_link}" = "Hello from Rufux macOS ${fstype} RW mount!" ] || fail "${fstype}: symlink content mismatch"

  # Directory listing check via ls -la
  local ls_out
  ls_out="$(ls -la "${mnt}/subdir")"
  echo "${ls_out}" | grep -F "hello world.txt" >/dev/null || fail "${fstype}: 'hello world.txt' missing in ls -la"
  echo "${ls_out}" | grep -F "blob_final.bin" >/dev/null || fail "${fstype}: 'blob_final.bin' missing in ls -la"
  echo "${ls_out}" | grep -F "hello_symlink.txt" >/dev/null || fail "${fstype}: 'hello_symlink.txt' missing in ls -la"

  # 2b. Verify idempotent double-mount returns existing mountpoint without spawning a 2nd daemon
  local pid_before
  pid_before="$(cat "${STATE_DIR}"/*.pid | head -n 1)"
  [ -n "${pid_before}" ] && kill -0 "${pid_before}" 2>/dev/null || fail "${fstype}: expected active rufux-mountd pid"
  local mnt_dup
  mnt_dup="$("${CLI}" mount "${img}")"
  [ "${mnt_dup}" = "${mnt}" ] || fail "${fstype}: idempotent double-mount returned '${mnt_dup}', expected '${mnt}'"
  local pid_count
  pid_count="$(ls -1 "${STATE_DIR}"/*.pid 2>/dev/null | wc -l | tr -d ' ')"
  [ "${pid_count}" = "1" ] || fail "${fstype}: expected exactly 1 pidfile after double-mount, got ${pid_count}"

  # 2c. Verify direct duplicate rufux-mountd invocation is rejected by exclusive device lock
  local mountd_kind="ext"
  [ "${fstype}" = "ntfs" ] && mountd_kind="ntfs"
  if "${MOUNTD_BIN}" --device "${img}" --fstype "${mountd_kind}" >/dev/null 2>&1; then
    fail "${fstype}: duplicate rufux-mountd must fail exclusive device lock while volume is mounted"
  fi

  # 3. Unmount by image path (containing spaces)
  "${CLI}" umount "${img}"
  [ ! -d "${mnt}" ] || fail "${fstype}: mount directory '${mnt}' was not removed on umount"

  # 4. Re-mount a second time to prove persistence to disk, verify SHA-256, test external-eject recovery, and run fsck!
  local mnt2
  mnt2="$("${CLI}" mount "${img}")"
  [ -n "${mnt2}" ] || fail "${fstype}: second mount returned empty mountpoint"
  [ -d "${mnt2}" ] || fail "${fstype}: second mountpoint '${mnt2}' is not a directory"
  ACTIVE_MNTS+=("${mnt2}")

  [ -f "${mnt2}/subdir/hello world.txt" ] || fail "${fstype}: persisted 'hello world.txt' missing on remount"
  [ ! -e "${mnt2}/subdir/temp_to_delete.bin" ] || fail "${fstype}: deleted file reappeared on remount"
  [ ! -e "${mnt2}/subdir/blob_orig.bin" ] || fail "${fstype}: renamed-from file reappeared on remount"
  [ -L "${mnt2}/subdir/hello_symlink.txt" ] || fail "${fstype}: persisted symlink missing on remount"

  local got_txt2
  got_txt2="$(cat "${mnt2}/subdir/hello world.txt")"
  [ "${got_txt2}" = "Hello from Rufux macOS ${fstype} RW mount!" ] || fail "${fstype}: persisted text mismatch: '${got_txt2}'"

  local sha2
  sha2="$(shasum -a 256 "${mnt2}/subdir/blob_final.bin" | awk '{print $1}')"
  [ "${sha2}" = "${WANT_SHA}" ] || fail "${fstype}: persisted 2 MiB SHA-256 mismatch (${sha2} != ${WANT_SHA})"

  # 4b. Simulate external eject (/sbin/umount outside rufux_unmount) leaving rufux-mountd alive,
  #     then verify rufux_mount cleans up the orphaned daemon/state and remounts cleanly!
  printf 'persisted across external umount\n' > "${mnt2}/subdir/external_eject.txt"
  local orphan_pid
  orphan_pid="$(cat "${STATE_DIR}"/*.pid | head -n 1)"
  /sbin/umount "${mnt2}"
  local mnt3
  mnt3="$("${CLI}" mount "${img}")"
  [ -n "${mnt3}" ] && [ -d "${mnt3}" ] || fail "${fstype}: remount after external /sbin/umount failed"
  ACTIVE_MNTS+=("${mnt3}")
  if [ -n "${orphan_pid}" ] && kill -0 "${orphan_pid}" 2>/dev/null; then
    fail "${fstype}: orphaned rufux-mountd pid ${orphan_pid} was not terminated before remount"
  fi
  [ "$(cat "${mnt3}/subdir/external_eject.txt")" = "persisted across external umount" ] || \
    fail "${fstype}: external_eject.txt did not persist across external umount cleanup"

  # Unmount third pass by mountpoint path (also containing spaces)
  "${CLI}" umount "${mnt3}"
  [ ! -d "${mnt3}" ] || fail "${fstype}: third mount directory '${mnt3}' was not removed on umount"

  # 5. Filesystem integrity verification (zero corruption!)
  if [ "${fstype}" = "ntfs" ]; then
    ntfsfix -n "${img}" >/dev/null || fail "ntfsfix -n reported errors on ${img}"
  else
    e2fsck -fn "${img}" >/dev/null || fail "e2fsck -fn reported errors on ${img}"
  fi

  rm -f "${img}"
  echo "=== ${fstype}: PASS ==="
}

run_rw_persistence_test "ntfs" "RUFUX NTFS"
run_rw_persistence_test "ext2" "RUFUX EXT2"
run_rw_persistence_test "ext3" "RUFUX EXT3"
run_rw_persistence_test "ext4" "RUFUX EXT4"

# Unmount failure protection test (EPERM when unprivileged / EBUSY when non-root):
# When VFS unmount fails and mountpoint remains active in getmntinfo(), rufux_unmount
# must return non-zero, keep rufux-mountd alive, and preserve .state for retry!
echo "=== Testing NFS unmount failure preserves active daemon & state ==="
FAIL_IMG="${SPACE_DIR}/unmount_fail.img"
dd if=/dev/zero of="${FAIL_IMG}" bs=1048576 count=0 seek=16 status=none
mke2fs -q -t ext4 -F -L "UMNTFAIL" "${FAIL_IMG}" >/dev/null

FAIL_MNT="$("${CLI}" mount "${FAIL_IMG}")"
ACTIVE_MNTS+=("${FAIL_MNT}")
FAIL_STATE="$(ls -1 "${STATE_DIR}"/*.state | head -n 1)"
FAIL_PID="$(awk -F= '/^pid=/{print $2}' "${FAIL_STATE}")"

if [ "$(id -u)" -eq 0 ]; then
  chmod 755 "${TMP_DIR}" "${SPACE_DIR}" "${MNT_ROOT}"
  chmod 777 "${STATE_DIR}"
  chmod 666 "${FAIL_STATE}"
  if python3 -c '
import os, subprocess, sys
os.setgid(65534)
os.setuid(65534)
sys.exit(subprocess.call([sys.argv[1], "umount", sys.argv[2]]))
' "${CLI}" "${FAIL_MNT}" >/dev/null 2>&1; then
    fail "unprivileged umount on root-mounted NFS volume must fail with EPERM"
  fi
else
  ( cd "${FAIL_MNT}" && sleep 15 ) &
  HOLDER_PID=$!
  sleep 0.2
  if "${CLI}" umount "${FAIL_MNT}" >/dev/null 2>&1; then
    kill "${HOLDER_PID}" >/dev/null 2>&1 || true
    fail "umount on busy non-root NFS volume must fail"
  fi
  kill "${HOLDER_PID}" >/dev/null 2>&1 || true
  wait "${HOLDER_PID}" 2>/dev/null || true
fi

kill -0 "${FAIL_PID}" 2>/dev/null || fail "failed umount prematurely killed rufux-mountd"
[ -f "${FAIL_STATE}" ] || fail "failed umount deleted .state file while mount remained active"
[ -d "${FAIL_MNT}" ] || fail "failed umount removed active mount directory"

# Now perform genuine unmount as owner and verify clean flush + e2fsck
"${CLI}" umount "${FAIL_MNT}"
[ ! -d "${FAIL_MNT}" ] || fail "mount directory not removed on retry umount"
if kill -0 "${FAIL_PID}" 2>/dev/null; then
  fail "rufux-mountd pid ${FAIL_PID} still alive after successful retry umount"
fi
e2fsck -fn "${FAIL_IMG}" >/dev/null || fail "e2fsck failed on unmount_fail.img"
rm -f "${FAIL_IMG}"
echo "=== NFS unmount failure protection: PASS ==="

# Native FAT32 fallback test on an attached virtual block device (/dev/diskN)
echo "=== Testing native FAT32 fallback mount/umount ==="
FAT_IMG="${SPACE_DIR}/fat32.img"
dd if=/dev/zero of="${FAT_IMG}" bs=1048576 count=0 seek=64 status=none
ATTACH_OUT="$(/usr/bin/hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount -readwrite "${FAT_IMG}")"
FAT_DISK="$(echo "${ATTACH_OUT}" | awk '/^\/dev\/disk[0-9]+/{print $1; exit}')"
[ -n "${FAT_DISK}" ] || fail "failed to attach raw FAT32 test disk"
ATTACHED_DISKS+=("${FAT_DISK}")
FAT_RDISK="/dev/r${FAT_DISK#/dev/}"
/sbin/newfs_msdos -F 32 -v "RUFUXFAT" "${FAT_RDISK}" >/dev/null

FAT_MNT="$("${CLI}" mount "${FAT_DISK}")"
[ -n "${FAT_MNT}" ] && [ -d "${FAT_MNT}" ] || fail "native FAT32 mount failed (got '${FAT_MNT}')"
ACTIVE_MNTS+=("${FAT_MNT}")
printf 'native fat32 ok\n' > "${FAT_MNT}/fat_probe.txt"
[ "$(cat "${FAT_MNT}/fat_probe.txt")" = "native fat32 ok" ] || fail "native FAT32 write/read failed"
"${CLI}" umount "${FAT_DISK}"

/usr/bin/hdiutil detach -force "${FAT_DISK}" >/dev/null
ATTACHED_DISKS=()
rm -f "${FAT_IMG}"
echo "=== native FAT32 fallback: PASS ==="

echo "test_mount_rw.sh: ALL MOUNT RW TESTS PASSED"

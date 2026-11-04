#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "boot.h"
#include "device.h"
#include "dosboot.h"
#include "exec.h"
#include "partition.h"
#include "platform_macos.h"

static char g_mounted_dir[PATH_MAX] = {0};
static char g_attached_disk[128] = {0};
static char g_tmpdir[PATH_MAX] = {0};

static void emergency_cleanup(void) {
  if (g_mounted_dir[0]) {
    char m[PATH_MAX];
    snprintf(m, sizeof m, "%s", g_mounted_dir);
    g_mounted_dir[0] = 0;
    char out[1024] = {0};
    const char *uav[] = {"/usr/sbin/diskutil", "unmount", "force", m, NULL};
    if (rufux_capture(uav, out, sizeof out) != 0) {
      const char *um[] = {"/sbin/umount", "-f", m, NULL};
      rufux_capture(um, out, sizeof out);
    }
  }
  if (g_attached_disk[0]) {
    char d[128];
    snprintf(d, sizeof d, "%s", g_attached_disk);
    g_attached_disk[0] = 0;
    rufux_macos_detach_disk(d);
  }
  if (g_tmpdir[0]) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", g_tmpdir);
    g_tmpdir[0] = 0;
    const char *av[] = {"/bin/rm", "-rf", dir, NULL};
    rufux_run(av, 0);
  }
}

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    emergency_cleanup(); \
    exit(1); \
  } \
} while (0)

static void make_tmpdir(char *out, size_t cap) {
  snprintf(out, cap, "/tmp/rufux-ctest-pboot-XXXXXX");
  char *d = mkdtemp(out);
  CHECK(d != NULL);
  snprintf(g_tmpdir, sizeof g_tmpdir, "%s", out);
}

static uint32_t rd_le32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_le64(const unsigned char *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

static uint32_t test_crc32(const unsigned char *buf, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= buf[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

static void create_zeroed_image(const char *path, unsigned long long size_bytes) {
  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
  CHECK(fd >= 0);
  CHECK(ftruncate(fd, (off_t)size_bytes) == 0);
  close(fd);
}

static void read_at_exact(const char *path, void *buf, size_t len, unsigned long long off) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  CHECK(fd >= 0);
  unsigned char *p = (unsigned char *)buf;
  size_t got = 0;
  while (got < len) {
    ssize_t r = pread(fd, p + got, len - got, (off_t)(off + got));
    if (r < 0 && errno == EINTR) continue;
    CHECK(r > 0);
    got += (size_t)r;
  }
  close(fd);
}

static void write_at_exact(const char *path, const void *buf, size_t len, unsigned long long off) {
  int fd = open(path, O_RDWR | O_CLOEXEC);
  CHECK(fd >= 0);
  CHECK(rufux_pwrite_all(fd, buf, len, (off_t)off) == 0);
  close(fd);
}

static int is_all_byte(const unsigned char *buf, size_t len, unsigned char val) {
  for (size_t i = 0; i < len; i++) {
    if (buf[i] != val) return 0;
  }
  return 1;
}

static int has_nonzero(const unsigned char *buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (buf[i] != 0) return 1;
  }
  return 0;
}

static void decode_utf16le_ascii(const unsigned char *utf16, size_t max_units, char *out, size_t cap) {
  size_t k = 0;
  for (size_t i = 0; i < max_units && k + 1 < cap; i++) {
    uint16_t u = (uint16_t)utf16[i * 2] | ((uint16_t)utf16[i * 2 + 1] << 8);
    if (u == 0) break;
    out[k++] = (u < 128) ? (char)u : '?';
  }
  out[k] = 0;
}

static void wait_for_slice_node(const char *slice_node) {
  for (int i = 0; i < 100; i++) {
    if (access(slice_node, F_OK) == 0) return;
    usleep(50000); // 50ms * 100 = 5s max
  }
  CHECK(access(slice_node, F_OK) == 0);
}

// Mixed-endian EFI GUID byte constants:
// Microsoft Basic Data: EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
static const unsigned char GUID_MS_BASIC_DATA[16] = {
  0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
  0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
};
// BIOS Boot: 21686148-6449-6E6F-744E-656564454649 ("Hah!IdontNeedEFI")
static const unsigned char GUID_BIOS_BOOT[16] = {
  0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6F, 0x6E,
  0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49
};
// Linux Filesystem Data: 0FC63DAF-8483-4772-8E79-3D69D8477DE4
static const unsigned char GUID_LINUX_FS[16] = {
  0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
  0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
};
// EFI System Partition: C12A7328-F81F-11D2-BA4B-00A0C93EC93B
static const unsigned char GUID_ESP[16] = {
  0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
  0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

static void verify_gpt_headers_and_crc(const char *img_path,
                                       unsigned long long total_lbas,
                                       unsigned char primary_hdr[512],
                                       unsigned char backup_hdr[512],
                                       unsigned char entries[16384]) {
  const unsigned long long last_lba = total_lbas - 1ULL;
  read_at_exact(img_path, primary_hdr, 512, 1ULL * 512ULL);
  read_at_exact(img_path, backup_hdr, 512, last_lba * 512ULL);

  CHECK(memcmp(primary_hdr, "EFI PART", 8) == 0);
  CHECK(memcmp(backup_hdr, "EFI PART", 8) == 0);

  CHECK(rd_le32(primary_hdr + 8) == 0x00010000u);
  CHECK(rd_le32(primary_hdr + 12) == 92u);
  CHECK(rd_le32(primary_hdr + 20) == 0u);
  CHECK(rd_le64(primary_hdr + 24) == 1ULL);
  CHECK(rd_le64(primary_hdr + 32) == last_lba);
  CHECK(rd_le64(primary_hdr + 40) == 34ULL);
  CHECK(rd_le64(primary_hdr + 48) == last_lba - 33ULL);
  CHECK(rd_le64(primary_hdr + 72) == 2ULL);
  CHECK(rd_le32(primary_hdr + 80) == 128u);
  CHECK(rd_le32(primary_hdr + 84) == 128u);

  CHECK(rd_le32(backup_hdr + 8) == 0x00010000u);
  CHECK(rd_le32(backup_hdr + 12) == 92u);
  CHECK(rd_le32(backup_hdr + 20) == 0u);
  CHECK(rd_le64(backup_hdr + 24) == last_lba);
  CHECK(rd_le64(backup_hdr + 32) == 1ULL);
  CHECK(rd_le64(backup_hdr + 40) == 34ULL);
  CHECK(rd_le64(backup_hdr + 48) == last_lba - 33ULL);
  CHECK(rd_le64(backup_hdr + 72) == last_lba - 32ULL);
  CHECK(rd_le32(backup_hdr + 80) == 128u);
  CHECK(rd_le32(backup_hdr + 84) == 128u);

  // Disk GUID identical between primary & backup, with RFC-4122 v4 bits.
  CHECK(has_nonzero(primary_hdr + 56, 16));
  CHECK(memcmp(primary_hdr + 56, backup_hdr + 56, 16) == 0);
  CHECK((primary_hdr[56 + 7] & 0xF0u) == 0x40u);
  CHECK((primary_hdr[56 + 8] & 0xC0u) == 0x80u);

  // Primary & backup entry arrays match and match header CRC32.
  unsigned char backup_entries[16384];
  read_at_exact(img_path, entries, sizeof(backup_entries), 2ULL * 512ULL);
  read_at_exact(img_path, backup_entries, sizeof(backup_entries), (last_lba - 32ULL) * 512ULL);
  CHECK(memcmp(entries, backup_entries, sizeof(backup_entries)) == 0);

  uint32_t expected_ent_crc = test_crc32(entries, sizeof(backup_entries));
  CHECK(rd_le32(primary_hdr + 88) == expected_ent_crc);
  CHECK(rd_le32(backup_hdr + 88) == expected_ent_crc);

  // Primary & backup header CRC32s (over 92 bytes with 16..19 zeroed) match.
  unsigned char hcopy[92];
  memcpy(hcopy, primary_hdr, 92);
  memset(hcopy + 16, 0, 4);
  CHECK(rd_le32(primary_hdr + 16) == test_crc32(hcopy, 92));

  memcpy(hcopy, backup_hdr, 92);
  memset(hcopy + 16, 0, 4);
  CHECK(rd_le32(backup_hdr + 16) == test_crc32(hcopy, 92));
}

static void test_safety_and_validation_guards(const char *tmpdir) {
  char img[PATH_MAX];
  snprintf(img, sizeof img, "%s/guard_test.img", tmpdir);
  create_zeroed_image(img, 16ULL * 1024ULL * 1024ULL);

  char err[512] = {0};
  RufuxPartOpts po = {
    .scheme = "gpt",
    .layout = "single",
    .fs_main = "vfat",
    .bios_boot = 0,
    .dry_run = 0,
    .allow_fixed = 0,
    .allow_file = 1,
    .yes = 0
  };

  // Must refuse real partition without --yes.
  CHECK(rufux_partition(img, &po, err, sizeof err) == -1);
  CHECK(strstr(err, "--yes") != NULL);

  // Must refuse regular file without --allow-file.
  po.yes = 1;
  po.allow_file = 0;
  CHECK(rufux_partition(img, &po, err, sizeof err) == -1);

  // Must unconditionally refuse boot disk /dev/disk0 even with allow_fixed=1, yes=1.
  po.allow_fixed = 1;
  CHECK(rufux_partition("/dev/disk0", &po, err, sizeof err) == -1);

  // Invalid scheme and invalid layout must be refused.
  po.allow_file = 1;
  po.scheme = "apple_partition_map";
  CHECK(rufux_partition(img, &po, err, sizeof err) == -1);
  po.scheme = "gpt";
  po.layout = "invalid_layout";
  CHECK(rufux_partition(img, &po, err, sizeof err) == -1);

  // main+uefintfs on < 64 MiB must be refused.
  po.layout = "main+uefintfs";
  CHECK(rufux_partition(img, &po, err, sizeof err) == -1);
  CHECK(strstr(err, "64 MiB") != NULL);

  // Boot installer guards: refuse without --yes, refuse without --allow-file, refuse /dev/disk0.
  RufuxBootOpts bo = {.kind = "bios", .dry_run = 0, .allow_fixed = 1, .allow_file = 1, .yes = 0};
  CHECK(rufux_install_mbr(img, &bo, err, sizeof err) == -1);
  bo.yes = 1;
  bo.allow_file = 0;
  CHECK(rufux_install_mbr(img, &bo, err, sizeof err) == -1);
  CHECK(rufux_install_mbr("/dev/disk0", &bo, err, sizeof err) == -1);

  if (rufux_have("syslinux")) {
    CHECK(rufux_install_syslinux(img, /*dry_run=*/1, err, sizeof err) == 0);
  } else {
    err[0] = 0;
    CHECK(rufux_install_syslinux(img, /*dry_run=*/1, err, sizeof err) == -1);
    CHECK(strstr(err, "syslinux") != NULL);
  }
}

static void test_gpt_main_uefintfs_biosboot_and_hdiutil(const char *tmpdir) {
  // Known IEEE CRC32 sanity check ("123456789" -> 0xCBF43926).
  CHECK(test_crc32((const unsigned char *)"123456789", 9) == 0xCBF43926u);

  const unsigned long long disk_bytes = 128ULL * 1024ULL * 1024ULL; // 128 MiB
  const unsigned long long total_lbas = disk_bytes / 512ULL;        // 262144
  const unsigned long long last_lba = total_lbas - 1ULL;            // 262143

  char img[PATH_MAX];
  snprintf(img, sizeof img, "%s/gpt_win_128m.img", tmpdir);
  create_zeroed_image(img, disk_bytes);

  // Pre-poison stale superblock bytes at partition 1 head (LBA 2048), partition 1 tail (LBA 255999),
  // partition 2 (LBA 256000), and partition 3 (LBA 258048) to prove partition-edge wiping!
  unsigned char poison[512];
  memset(poison, 0xA5, sizeof poison);
  memcpy(poison + 3, "NTFS    ", 8);
  write_at_exact(img, poison, sizeof poison, 2048ULL * 512ULL);
  write_at_exact(img, poison, sizeof poison, 255999ULL * 512ULL);
  write_at_exact(img, poison, sizeof poison, 256000ULL * 512ULL);
  write_at_exact(img, poison, sizeof poison, 258048ULL * 512ULL);
  // Also poison outside partition extents at LBA 10000 (inside middle of p1, >1MiB from edges)
  // to confirm wiping targets only the metadata/edge regions, not slow full-disk zeroing.
  write_at_exact(img, poison, sizeof poison, 10000ULL * 512ULL);

  RufuxPartOpts po = {
    .scheme = "gpt",
    .layout = "main+uefintfs",
    .fs_main = "ntfs",
    .bios_boot = 1,
    .dry_run = 1,
    .allow_fixed = 0,
    .allow_file = 1,
    .yes = 0
  };
  char plan[256] = {0};
  rufux_partition_plan(img, &po, plan, sizeof plan);
  CHECK(strstr(plan, "gpt") != NULL && strstr(plan, "main+uefintfs") != NULL);

  char err[512] = {0};
  // dry_run=1 must not touch the image.
  CHECK(rufux_partition(img, &po, err, sizeof err) == 0);
  unsigned char check_sec[512] = {0};
  read_at_exact(img, check_sec, sizeof check_sec, 2048ULL * 512ULL);
  CHECK(memcmp(check_sec, poison, sizeof poison) == 0);

  // Real partition run.
  po.dry_run = 0;
  po.yes = 1;
  CHECK(rufux_partition(img, &po, err, sizeof err) == 0);

  // 1. Verify protective MBR at LBA 0.
  unsigned char mbr[512] = {0};
  read_at_exact(img, mbr, sizeof mbr, 0);
  CHECK(mbr[510] == 0x55u && mbr[511] == 0xAAu);
  CHECK(has_nonzero(mbr, 440)); // mbr_gpt_syslinux bootstrap
  const unsigned char *pe0 = mbr + 446;
  CHECK(pe0[0] == 0x00u);
  CHECK(pe0[4] == 0xEEu);
  CHECK(rd_le32(pe0 + 8) == 1u);
  CHECK(rd_le32(pe0 + 12) == (uint32_t)last_lba);
  CHECK(is_all_byte(mbr + 462, 48, 0x00u));

  // 2. Verify primary & backup GPT headers and IEEE CRC32s.
  unsigned char primary_hdr[512], backup_hdr[512], entries[16384];
  verify_gpt_headers_and_crc(img, total_lbas, primary_hdr, backup_hdr, entries);

  // 3. Verify Partition 1: Microsoft Basic Data, 124 MiB (LBA 2048..255999), name "Windows".
  const unsigned char *e1 = entries + 0 * 128;
  CHECK(memcmp(e1, GUID_MS_BASIC_DATA, 16) == 0);
  CHECK(has_nonzero(e1 + 16, 16));
  CHECK((e1[16 + 7] & 0xF0u) == 0x40u);
  CHECK((e1[16 + 8] & 0xC0u) == 0x80u);
  CHECK(rd_le64(e1 + 32) == 2048ULL);
  CHECK(rd_le64(e1 + 40) == 255999ULL);
  CHECK(rd_le64(e1 + 48) == 0ULL);
  char name1[64] = {0};
  decode_utf16le_ascii(e1 + 56, 36, name1, sizeof name1);
  CHECK(strcmp(name1, "Windows") == 0);

  // 4. Verify Partition 2: Microsoft Basic Data, 1 MiB (LBA 256000..258047),
  //    attributes 0x8000000000000000ULL (GUID:63), UTF-16LE name "UEFI:NTFS".
  const unsigned char *e2 = entries + 1 * 128;
  CHECK(memcmp(e2, GUID_MS_BASIC_DATA, 16) == 0);
  CHECK(has_nonzero(e2 + 16, 16));
  CHECK(memcmp(e1 + 16, e2 + 16, 16) != 0);
  CHECK((e2[16 + 7] & 0xF0u) == 0x40u);
  CHECK((e2[16 + 8] & 0xC0u) == 0x80u);
  CHECK(rd_le64(e2 + 32) == 256000ULL);
  CHECK(rd_le64(e2 + 40) == 258047ULL);
  CHECK(rd_le64(e2 + 48) == 0x8000000000000000ULL);
  char name2[64] = {0};
  decode_utf16le_ascii(e2 + 56, 36, name2, sizeof name2);
  CHECK(strcmp(name2, "UEFI:NTFS") == 0);

  // 5. Verify Partition 3: BIOS boot GUID 21686148-6449-6E6F-744E-656564454649,
  //    1 MiB (LBA 258048..260095), UTF-16LE name "BIOS boot".
  const unsigned char *e3 = entries + 2 * 128;
  CHECK(memcmp(e3, GUID_BIOS_BOOT, 16) == 0);
  CHECK(has_nonzero(e3 + 16, 16));
  CHECK(memcmp(e2 + 16, e3 + 16, 16) != 0);
  CHECK((e3[16 + 7] & 0xF0u) == 0x40u);
  CHECK((e3[16 + 8] & 0xC0u) == 0x80u);
  CHECK(rd_le64(e3 + 32) == 258048ULL);
  CHECK(rd_le64(e3 + 40) == 260095ULL);
  CHECK(rd_le64(e3 + 48) == 0ULL);
  char name3[64] = {0};
  decode_utf16le_ascii(e3 + 56, 36, name3, sizeof name3);
  CHECK(strcmp(name3, "BIOS boot") == 0);

  // Unused entries 4..128 must be zeroed.
  CHECK(is_all_byte(entries + 3 * 128, (128 - 3) * 128, 0x00u));

  // 6. Verify poisoned partition edges were wiped to 0x00!
  read_at_exact(img, check_sec, sizeof check_sec, 2048ULL * 512ULL);
  CHECK(is_all_byte(check_sec, sizeof check_sec, 0x00u));
  read_at_exact(img, check_sec, sizeof check_sec, 255999ULL * 512ULL);
  CHECK(is_all_byte(check_sec, sizeof check_sec, 0x00u));
  read_at_exact(img, check_sec, sizeof check_sec, 256000ULL * 512ULL);
  CHECK(is_all_byte(check_sec, sizeof check_sec, 0x00u));
  read_at_exact(img, check_sec, sizeof check_sec, 258048ULL * 512ULL);
  CHECK(is_all_byte(check_sec, sizeof check_sec, 0x00u));
  // Interior LBA 10000 untouched.
  read_at_exact(img, check_sec, sizeof check_sec, 10000ULL * 512ULL);
  CHECK(memcmp(check_sec, poison, sizeof poison) == 0);

  // 7. Verify rufux_part_offset_sectors on the image file.
  unsigned long long st = 0, cnt = 0;
  CHECK(rufux_part_offset_sectors(img, 1, &st, &cnt) == 0);
  CHECK(st == 2048ULL && cnt == 253952ULL);
  CHECK(rufux_part_offset_sectors(img, 2, &st, &cnt) == 0);
  CHECK(st == 256000ULL && cnt == 2048ULL);
  CHECK(rufux_part_offset_sectors(img, 3, &st, &cnt) == 0);
  CHECK(st == 258048ULL && cnt == 2048ULL);
  CHECK(rufux_part_offset_sectors(img, 4, &st, &cnt) == -1);
  CHECK(rufux_part_offset_sectors(img, 0, &st, &cnt) == -1);

  // 8. Attach via hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount
  //    and confirm macOS IOGUIDPartitionScheme recognizes all 3 slices!
  char disk_node[128] = {0};
  CHECK(rufux_macos_attach_raw_image(img, /*readonly=*/1, disk_node, sizeof disk_node) == 0);
  snprintf(g_attached_disk, sizeof g_attached_disk, "%s", disk_node);

  char s1[160], s2[160], s3[160], s4[160], rdisk[160];
  rufux_macos_part_slice(disk_node, 1, s1, sizeof s1);
  rufux_macos_part_slice(disk_node, 2, s2, sizeof s2);
  rufux_macos_part_slice(disk_node, 3, s3, sizeof s3);
  rufux_macos_part_slice(disk_node, 4, s4, sizeof s4);
  rufux_macos_raw_node(disk_node, rdisk, sizeof rdisk);

  wait_for_slice_node(s1);
  wait_for_slice_node(s2);
  wait_for_slice_node(s3);
  CHECK(access(s4, F_OK) != 0);

  CHECK(rufux_macos_dev_bytes(s1) == 124ULL * 1024ULL * 1024ULL);
  CHECK(rufux_macos_dev_bytes(s2) == 1ULL * 1024ULL * 1024ULL);
  CHECK(rufux_macos_dev_bytes(s3) == 1ULL * 1024ULL * 1024ULL);

  // rufux_part_offset_sectors must also work on /dev/diskK and /dev/rdiskK!
  CHECK(rufux_part_offset_sectors(disk_node, 1, &st, &cnt) == 0);
  CHECK(st == 2048ULL && cnt == 253952ULL);
  CHECK(rufux_part_offset_sectors(rdisk, 2, &st, &cnt) == 0);
  CHECK(st == 256000ULL && cnt == 2048ULL);
  CHECK(rufux_part_offset_sectors(rdisk, 3, &st, &cnt) == 0);
  CHECK(st == 258048ULL && cnt == 2048ULL);

  CHECK(rufux_macos_detach_disk(disk_node) == 0);
  g_attached_disk[0] = 0;
}

static void test_gpt_single_ext4_and_esp_main(const char *tmpdir) {
  const unsigned long long disk_bytes = 128ULL * 1024ULL * 1024ULL;
  const unsigned long long total_lbas = disk_bytes / 512ULL;

  char img[PATH_MAX];
  snprintf(img, sizeof img, "%s/gpt_ext4_128m.img", tmpdir);
  create_zeroed_image(img, disk_bytes);

  char err[512] = {0};
  RufuxPartOpts po = {
    .scheme = "gpt",
    .layout = "single",
    .fs_main = "ext4",
    .bios_boot = 0,
    .dry_run = 0,
    .allow_fixed = 0,
    .allow_file = 1,
    .yes = 1
  };
  CHECK(rufux_partition(img, &po, err, sizeof err) == 0);

  unsigned char primary_hdr[512], backup_hdr[512], entries[16384];
  verify_gpt_headers_and_crc(img, total_lbas, primary_hdr, backup_hdr, entries);

  const unsigned char *e1 = entries;
  CHECK(memcmp(e1, GUID_LINUX_FS, 16) == 0);
  CHECK(rd_le64(e1 + 32) == 2048ULL);
  CHECK(rd_le64(e1 + 40) == total_lbas - 34ULL);
  CHECK(is_all_byte(entries + 128, (128 - 1) * 128, 0x00u));

  unsigned long long st = 0, cnt = 0;
  CHECK(rufux_part_offset_sectors(img, 1, &st, &cnt) == 0);
  CHECK(st == 2048ULL && cnt == (total_lbas - 34ULL) - 2048ULL + 1ULL);
  CHECK(rufux_part_offset_sectors(img, 2, &st, &cnt) == -1);

  // Also test gpt + esp+main on a sparse 600 MiB image.
  char esp_img[PATH_MAX];
  snprintf(esp_img, sizeof esp_img, "%s/gpt_esp_600m.img", tmpdir);
  const unsigned long long esp_disk_bytes = 600ULL * 1024ULL * 1024ULL;
  const unsigned long long esp_total_lbas = esp_disk_bytes / 512ULL;
  create_zeroed_image(esp_img, esp_disk_bytes);

  po.layout = "esp+main";
  po.fs_main = "ntfs";
  CHECK(rufux_partition(esp_img, &po, err, sizeof err) == 0);
  verify_gpt_headers_and_crc(esp_img, esp_total_lbas, primary_hdr, backup_hdr, entries);

  CHECK(memcmp(entries + 0 * 128, GUID_ESP, 16) == 0);
  CHECK(rd_le64(entries + 0 * 128 + 32) == 2048ULL);
  CHECK(rd_le64(entries + 0 * 128 + 40) == 2048ULL + 512ULL * 2048ULL - 1ULL);

  CHECK(memcmp(entries + 1 * 128, GUID_MS_BASIC_DATA, 16) == 0);
  CHECK(rd_le64(entries + 1 * 128 + 32) == 513ULL * 2048ULL);
  CHECK(rd_le64(entries + 1 * 128 + 40) == esp_total_lbas - 34ULL);
}

static void test_dos_main_uefintfs_and_install_mbr(const char *tmpdir) {
  const unsigned long long disk_bytes = 128ULL * 1024ULL * 1024ULL;
  char img[PATH_MAX];
  snprintf(img, sizeof img, "%s/dos_win_128m.img", tmpdir);
  create_zeroed_image(img, disk_bytes);

  char err[512] = {0};
  RufuxPartOpts po = {
    .scheme = "dos",
    .layout = "main+uefintfs",
    .fs_main = "ntfs",
    .bios_boot = 1, // ignored on dos
    .dry_run = 0,
    .allow_fixed = 0,
    .allow_file = 1,
    .yes = 1
  };
  CHECK(rufux_partition(img, &po, err, sizeof err) == 0);

  unsigned char mbr[512] = {0};
  read_at_exact(img, mbr, sizeof mbr, 0);

  // MBR bootstrap bytes non-zero (mbr_syslinux starts with 0x33, 0xC0, 0xFA).
  CHECK(has_nonzero(mbr, 440));
  CHECK(mbr[0] == 0x33u && mbr[1] == 0xC0u);
  CHECK(mbr[510] == 0x55u && mbr[511] == 0xAAu);

  // Partition 1: active 0x80, type 0x07 (NTFS), start 2048, size 125 MiB (256000 sectors).
  const unsigned char *p1 = mbr + 446;
  CHECK(p1[0] == 0x80u);
  CHECK(p1[4] == 0x07u);
  CHECK(rd_le32(p1 + 8) == 2048u);
  CHECK(rd_le32(p1 + 12) == 125u * 2048u);

  // Partition 2: status 0x00, type 0xef, start 126 MiB (258048), size 1 MiB (2048 sectors).
  const unsigned char *p2 = mbr + 462;
  CHECK(p2[0] == 0x00u);
  CHECK(p2[4] == 0xEFu);
  CHECK(rd_le32(p2 + 8) == 126u * 2048u);
  CHECK(rd_le32(p2 + 12) == 2048u);

  // Partitions 3 and 4 empty.
  CHECK(is_all_byte(mbr + 478, 32, 0x00u));

  unsigned long long st = 0, cnt = 0;
  CHECK(rufux_part_offset_sectors(img, 1, &st, &cnt) == 0);
  CHECK(st == 2048ULL && cnt == 256000ULL);
  CHECK(rufux_part_offset_sectors(img, 2, &st, &cnt) == 0);
  CHECK(st == 258048ULL && cnt == 2048ULL);
  CHECK(rufux_part_offset_sectors(img, 3, &st, &cnt) == -1);

  // Test rufux_install_mbr (gpt and bios) preserving bytes 440..511!
  unsigned char saved_tail[72];
  memcpy(saved_tail, mbr + 440, 72);

  unsigned char zeros440[440] = {0};
  write_at_exact(img, zeros440, sizeof zeros440, 0);

  RufuxBootOpts bo = {.kind = "gpt", .dry_run = 1, .allow_fixed = 0, .allow_file = 1, .yes = 0};
  CHECK(rufux_install_mbr(img, &bo, err, sizeof err) == 0);
  read_at_exact(img, mbr, sizeof mbr, 0);
  CHECK(is_all_byte(mbr, 440, 0x00u)); // dry_run untouched

  bo.dry_run = 0;
  bo.yes = 1;
  CHECK(rufux_install_mbr(img, &bo, err, sizeof err) == 0);
  unsigned char gpt_mbr[512] = {0};
  read_at_exact(img, gpt_mbr, sizeof gpt_mbr, 0);
  CHECK(has_nonzero(gpt_mbr, 440));
  CHECK(memcmp(gpt_mbr + 440, saved_tail, 72) == 0);

  bo.kind = "bios";
  CHECK(rufux_install_mbr(img, &bo, err, sizeof err) == 0);
  unsigned char bios_mbr[512] = {0};
  read_at_exact(img, bios_mbr, sizeof bios_mbr, 0);
  CHECK(has_nonzero(bios_mbr, 440));
  CHECK(memcmp(bios_mbr, gpt_mbr, 440) != 0); // bios != gpt bootstrap
  CHECK(memcmp(bios_mbr + 440, saved_tail, 72) == 0);

  // Also verify "mbr" scheme alias + fat16 -> 0x0e.
  char alias_img[PATH_MAX];
  snprintf(alias_img, sizeof alias_img, "%s/mbr_alias_32m.img", tmpdir);
  create_zeroed_image(alias_img, 32ULL * 1024ULL * 1024ULL);
  po.scheme = "mbr";
  po.layout = "single";
  po.fs_main = "fat16";
  CHECK(rufux_partition(alias_img, &po, err, sizeof err) == 0);
  read_at_exact(alias_img, mbr, sizeof mbr, 0);
  CHECK(mbr[446] == 0x80u && mbr[446 + 4] == 0x0Eu);
}

static void test_dosboot_end_to_end(const char *tmpdir) {
  // 1. rufux_freedos_dir() resolves in-tree res/freedos.
  const char *fd_dir = rufux_freedos_dir();
  CHECK(fd_dir != NULL);
  CHECK(strstr(fd_dir, "res/freedos") != NULL);

  char kpath[PATH_MAX], cpath[PATH_MAX];
  snprintf(kpath, sizeof kpath, "%s/KERNEL.SYS", fd_dir);
  snprintf(cpath, sizeof cpath, "%s/COMMAND.COM", fd_dir);
  CHECK(access(kpath, R_OK) == 0);
  CHECK(access(cpath, R_OK) == 0);

  // Test RUFUX_RES override as well.
  char custom_res[PATH_MAX], custom_fd[PATH_MAX];
  snprintf(custom_res, sizeof custom_res, "%s/custom_res", tmpdir);
  snprintf(custom_fd, sizeof custom_fd, "%s/custom_res/freedos", tmpdir);
  CHECK(mkdir(custom_res, 0755) == 0);
  CHECK(mkdir(custom_fd, 0755) == 0);
  const char *prev_res = getenv("RUFUX_RES");
  char *saved_res = prev_res ? strdup(prev_res) : NULL;
  CHECK(setenv("RUFUX_RES", custom_res, 1) == 0);
  const char *overridden = rufux_freedos_dir();
  CHECK(overridden != NULL && strstr(overridden, "custom_res/freedos") != NULL);
  if (saved_res) {
    CHECK(setenv("RUFUX_RES", saved_res, 1) == 0);
    free(saved_res);
  } else {
    CHECK(unsetenv("RUFUX_RES") == 0);
  }
  // Re-resolve in-tree res/freedos.
  char real_fd_dir[PATH_MAX];
  fd_dir = rufux_freedos_dir();
  CHECK(fd_dir != NULL);
  snprintf(real_fd_dir, sizeof real_fd_dir, "%s", fd_dir);

  // 2. Partition a 128 MiB image as DOS/single (vfat), attach via hdiutil, format slice 1
  //    as FAT32, mount it, copy FreeDOS files, and verify KERNEL.SYS is the very first
  //    readdir file entry!
  const unsigned long long disk_bytes = 128ULL * 1024ULL * 1024ULL;
  char img[PATH_MAX], mnt[PATH_MAX];
  snprintf(img, sizeof img, "%s/freedos_128m.img", tmpdir);
  snprintf(mnt, sizeof mnt, "%s/fat_mnt", tmpdir);
  create_zeroed_image(img, disk_bytes);
  CHECK(mkdir(mnt, 0755) == 0);

  char err[512] = {0};
  RufuxPartOpts po = {
    .scheme = "dos",
    .layout = "single",
    .fs_main = "vfat",
    .bios_boot = 0,
    .dry_run = 0,
    .allow_fixed = 0,
    .allow_file = 1,
    .yes = 1
  };
  CHECK(rufux_partition(img, &po, err, sizeof err) == 0);

  // Record exact partition entries 446..509 before dosboot writes.
  unsigned char orig_mbr[512] = {0};
  read_at_exact(img, orig_mbr, sizeof orig_mbr, 0);
  unsigned char orig_ptbl[64];
  memcpy(orig_ptbl, orig_mbr + 446, sizeof orig_ptbl);
  CHECK(has_nonzero(orig_ptbl, 16));

  char disk_node[128] = {0};
  CHECK(rufux_macos_attach_raw_image(img, /*readonly=*/0, disk_node, sizeof disk_node) == 0);
  snprintf(g_attached_disk, sizeof g_attached_disk, "%s", disk_node);

  char s1[160], rs1[160], rdisk[160];
  rufux_macos_part_slice(disk_node, 1, s1, sizeof s1);
  rufux_macos_raw_node(s1, rs1, sizeof rs1);
  rufux_macos_raw_node(disk_node, rdisk, sizeof rdisk);
  wait_for_slice_node(s1);

  char out[2048] = {0};
  const char *mkfs_av[] = {"/sbin/newfs_msdos", "-F", "32", "-v", "FREEDOS", rs1, NULL};
  CHECK(rufux_capture(mkfs_av, out, sizeof out) == 0);

  // Ensure diskarbitrationd didn't grab s1 before mounting at our private mountpoint.
  CHECK(rufux_macos_unmount_disk(disk_node, err, sizeof err) == 0);
  const char *mnt_av[] = {"/usr/sbin/diskutil", "mount", "-mountPoint", mnt, s1, NULL};
  CHECK(rufux_capture(mnt_av, out, sizeof out) == 0);
  snprintf(g_mounted_dir, sizeof g_mounted_dir, "%s", mnt);

  CHECK(rufux_dos_copy_files(real_fd_dir, mnt, err, sizeof err) == 0);

  // Verify readdir file order on the FAT32 mount: KERNEL.SYS MUST be the very first file entry!
  DIR *d = opendir(mnt);
  CHECK(d != NULL);
  char first_file[256] = {0};
  char second_file[256] = {0};
  int file_count = 0;
  int saw_readme = 0;
  int saw_appledouble = 0;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
    if (strncmp(de->d_name, "._", 2) == 0) saw_appledouble = 1;
    char full[PATH_MAX];
    snprintf(full, sizeof full, "%s/%s", mnt, de->d_name);
    struct stat st;
    CHECK(lstat(full, &st) == 0);
    if (!S_ISREG(st.st_mode)) continue; // skip directories such as .fseventsd
    if (strcasecmp(de->d_name, "readme.txt") == 0) saw_readme = 1;
    if (file_count == 0) snprintf(first_file, sizeof first_file, "%s", de->d_name);
    else if (file_count == 1) snprintf(second_file, sizeof second_file, "%s", de->d_name);
    file_count++;
  }
  closedir(d);

  CHECK(saw_appledouble == 0);
  CHECK(saw_readme == 0);
  CHECK(file_count >= 25);
  CHECK(strcmp(first_file, "KERNEL.SYS") == 0);
  CHECK(strcmp(second_file, "COMMAND.COM") == 0);

  // Unmount FAT32 slice, then test rufux_dos_pbr_fd32 and rufux_dos_mbr directly on
  // raw /dev/rdisk* nodes (verifying dosboot transparently uses buffered /dev/disk* I/O!).
  const char *umnt_av[] = {"/usr/sbin/diskutil", "unmount", "force", mnt, NULL};
  CHECK(rufux_capture(umnt_av, out, sizeof out) == 0);
  g_mounted_dir[0] = 0;

  CHECK(rufux_dos_pbr_fd32(rs1, 0, "fdboot", err, sizeof err) == 0);
  CHECK(rufux_dos_mbr(rdisk, err, sizeof err) == 0);

  CHECK(rufux_macos_detach_disk(disk_node) == 0);
  g_attached_disk[0] = 0;

  // 3. Inspect image bytes after detach:
  //    - DOS MBR bootstrap written at 0..439 (0xFA, 0x33, 0xC0)
  //    - Partition entries 446..509 preserved byte-for-byte!
  //    - 0x55AA preserved at 510..511
  unsigned char after_mbr[512] = {0};
  read_at_exact(img, after_mbr, sizeof after_mbr, 0);
  CHECK(after_mbr[0] == 0xFAu && after_mbr[1] == 0x33u && after_mbr[2] == 0xC0u);
  CHECK(memcmp(after_mbr + 446, orig_ptbl, 64) == 0);
  CHECK(after_mbr[510] == 0x55u && after_mbr[511] == 0xAAu);

  //    - FAT32 FreeDOS PBR at LBA 2048 (offset 1 MiB):
  //      head jump + "MSWIN4.1", BPB bytes_per_sector (512) preserved at +0x0B,
  //      uppercase 11-char padded label "FDBOOT     " at +0x47,
  //      "FAT32   " + FreeDOS boot code at +0x52, and 0x55AA at +0x1FE and +0x5FE.
  unsigned char pbr[1536] = {0};
  const unsigned long long p1_off = 2048ULL * 512ULL;
  read_at_exact(img, pbr, sizeof pbr, p1_off);
  CHECK(pbr[0] == 0xEBu && pbr[1] == 0x58u && pbr[2] == 0x90u);
  CHECK(memcmp(pbr + 3, "MSWIN4.1", 8) == 0);
  CHECK(pbr[0x0B] == 0x00u && pbr[0x0C] == 0x02u); // 512 bytes/sector BPB preserved!
  CHECK(memcmp(pbr + 0x47, "FDBOOT     ", 11) == 0);
  CHECK(memcmp(pbr + 0x52, "FAT32   ", 8) == 0);
  CHECK(pbr[0x1FE] == 0x55u && pbr[0x1FF] == 0xAAu);
  CHECK(pbr[0x5FE] == 0x55u && pbr[0x5FF] == 0xAAu);

  // 4. Test rufux_ntfs_pbr at base_off = 1 MiB preserving synthetic BPB bytes 0x0B..0x53.
  char ntfs_img[PATH_MAX];
  snprintf(ntfs_img, sizeof ntfs_img, "%s/ntfs_pbr_2m.img", tmpdir);
  create_zeroed_image(ntfs_img, 2ULL * 1024ULL * 1024ULL);
  unsigned char synth_bpb[0x54 - 0x0B];
  for (size_t i = 0; i < sizeof synth_bpb; i++) synth_bpb[i] = (unsigned char)(0x30u + (i & 0x3Fu));
  write_at_exact(ntfs_img, synth_bpb, sizeof synth_bpb, p1_off + 0x0BULL);

  CHECK(rufux_ntfs_pbr(ntfs_img, p1_off, err, sizeof err) == 0);
  unsigned char ntfs_br[4096] = {0};
  read_at_exact(ntfs_img, ntfs_br, sizeof ntfs_br, p1_off);
  CHECK(ntfs_br[0] == 0xEBu && ntfs_br[1] == 0x52u && ntfs_br[2] == 0x90u);
  CHECK(memcmp(ntfs_br + 3, "NTFS    ", 8) == 0);
  CHECK(memcmp(ntfs_br + 0x0B, synth_bpb, sizeof synth_bpb) == 0);
  CHECK(ntfs_br[0x54] == 0xFAu && ntfs_br[0x55] == 0x33u);
  CHECK(ntfs_br[0x1FE] == 0x55u && ntfs_br[0x1FF] == 0xAAu);
}

static void test_live_device_partition_and_repartition(const char *tmpdir) {
  const unsigned long long disk_bytes = 128ULL * 1024ULL * 1024ULL;
  char img[PATH_MAX];
  snprintf(img, sizeof img, "%s/live_repart_128m.img", tmpdir);
  create_zeroed_image(img, disk_bytes);

  char disk_node[128] = {0};
  CHECK(rufux_macos_attach_raw_image(img, /*readonly=*/0, disk_node, sizeof disk_node) == 0);
  snprintf(g_attached_disk, sizeof g_attached_disk, "%s", disk_node);

  char s1[160], s2[160], s3[160], rdisk[160];
  rufux_macos_part_slice(disk_node, 1, s1, sizeof s1);
  rufux_macos_part_slice(disk_node, 2, s2, sizeof s2);
  rufux_macos_part_slice(disk_node, 3, s3, sizeof s3);
  rufux_macos_raw_node(disk_node, rdisk, sizeof rdisk);

  // Initially blank: no slices exist.
  CHECK(access(s1, F_OK) != 0);

  // 1. Partition live /dev/diskK as GPT main+uefintfs + bios_boot (3 slices).
  //    Slices must already be settled and ready immediately upon return!
  char err[512] = {0};
  RufuxPartOpts po = {
    .scheme = "gpt",
    .layout = "main+uefintfs",
    .fs_main = "ntfs",
    .bios_boot = 1,
    .dry_run = 0,
    .allow_fixed = 1,
    .allow_file = 0,
    .yes = 1
  };
  CHECK(rufux_partition(disk_node, &po, err, sizeof err) == 0);
  CHECK(access(s1, F_OK) == 0);
  CHECK(access(s2, F_OK) == 0);
  CHECK(access(s3, F_OK) == 0);
  CHECK(rufux_macos_dev_bytes(s1) == 124ULL * 1024ULL * 1024ULL);
  CHECK(rufux_macos_dev_bytes(s2) == 1ULL * 1024ULL * 1024ULL);
  CHECK(rufux_macos_dev_bytes(s3) == 1ULL * 1024ULL * 1024ULL);

  // 2. Re-partition the SAME live /dev/rdiskK from 3-slice GPT -> 1-slice DOS,
  //    then install MBR directly onto live /dev/rdiskK.
  po.scheme = "dos";
  po.layout = "single";
  po.fs_main = "vfat";
  po.bios_boot = 0;
  CHECK(rufux_partition(rdisk, &po, err, sizeof err) == 0);
  CHECK(access(s2, F_OK) != 0);
  CHECK(access(s3, F_OK) != 0);
  CHECK(access(s1, F_OK) == 0);
  CHECK(rufux_macos_dev_bytes(s1) == 127ULL * 1024ULL * 1024ULL);

  RufuxBootOpts bo = {
    .kind = "bios",
    .dry_run = 0,
    .allow_fixed = 1,
    .allow_file = 0,
    .yes = 1
  };
  CHECK(rufux_install_mbr(rdisk, &bo, err, sizeof err) == 0);

  unsigned long long st = 0, cnt = 0;
  CHECK(rufux_part_offset_sectors(disk_node, 1, &st, &cnt) == 0);
  CHECK(st == 2048ULL && cnt == 127ULL * 2048ULL);

  CHECK(rufux_macos_detach_disk(disk_node) == 0);
  g_attached_disk[0] = 0;
}

int main(void) {
  atexit(emergency_cleanup);
  rufux_macos_init_bundle_env();

  char tmpdir[PATH_MAX];
  make_tmpdir(tmpdir, sizeof tmpdir);

  test_safety_and_validation_guards(tmpdir);
  test_gpt_main_uefintfs_biosboot_and_hdiutil(tmpdir);
  test_gpt_single_ext4_and_esp_main(tmpdir);
  test_dos_main_uefintfs_and_install_mbr(tmpdir);
  test_live_device_partition_and_repartition(tmpdir);
  test_dosboot_end_to_end(tmpdir);

  emergency_cleanup();
  printf("ctest_partition_boot: ALL TESTS PASSED\n");
  return 0;
}

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ntfs-3g/attrib.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/unistr.h>
#include <ntfs-3g/volume.h>

#include "device.h"
#include "exec.h"
#include "mkfs_task.h"
#include "ntfs_populate.h"
#include "partition.h"
#include "persist.h"
#include "platform_macos.h"

static char g_attached_disk[128] = {0};
static char g_tmpdir[PATH_MAX] = {0};

static void emergency_cleanup(void) {
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
    char out[512];
    rufux_capture(av, out, sizeof out);
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
  snprintf(out, cap, "/tmp/rufux-ctest-fs-XXXXXX");
  char *d = mkdtemp(out);
  CHECK(d != NULL);
  snprintf(g_tmpdir, sizeof g_tmpdir, "%s", out);
}

static void create_sparse_image(const char *path, unsigned long long size_bytes) {
  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0600);
  CHECK(fd >= 0);
  CHECK(ftruncate(fd, (off_t)size_bytes) == 0);
  CHECK(close(fd) == 0);
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

static void write_text_file(const char *path, const char *text) {
  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0644);
  CHECK(fd >= 0);
  size_t len = strlen(text);
  CHECK(rufux_pwrite_all(fd, text, len, 0) == 0);
  CHECK(close(fd) == 0);
}

static void mkdir_p(const char *path) {
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof tmp, "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      if (mkdir(tmp, 0755) != 0) CHECK(errno == EEXIST);
      *p = '/';
    }
  }
  if (mkdir(tmp, 0755) != 0) CHECK(errno == EEXIST);
}

static uint16_t rd_le16(const unsigned char *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
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

static void test_fs_helpers_and_guards(const char *tmpdir) {
  CHECK(rufux_fs_is_fat("vfat") == 1);
  CHECK(rufux_fs_is_fat("fat32") == 1);
  CHECK(rufux_fs_is_fat("fat16") == 1);
  CHECK(rufux_fs_is_fat("exfat") == 0);
  CHECK(rufux_fs_is_fat("ntfs") == 0);
  CHECK(rufux_fs_is_fat("ext4") == 0);
  CHECK(rufux_fs_is_fat("udf") == 0);
  CHECK(rufux_fs_is_fat("") == 0);
  CHECK(rufux_fs_is_fat(NULL) == 0);

  CHECK(rufux_fs_is_fat16("fat16") == 1);
  CHECK(rufux_fs_is_fat16("vfat") == 0);
  CHECK(rufux_fs_is_fat16("fat32") == 0);
  CHECK(rufux_fs_is_fat16(NULL) == 0);

  char img[PATH_MAX];
  snprintf(img, sizeof img, "%s/guards_64m.img", tmpdir);
  create_sparse_image(img, 64ULL * 1024ULL * 1024ULL);

  char err[512] = {0};
  RufuxMkfsOpts o = {
    .fs = NULL,
    .label = "TEST",
    .cluster_sectors = 0,
    .dry_run = 0,
    .allow_fixed = 0,
    .allow_file = 1,
    .yes = 1
  };

  // Missing --fs
  CHECK(rufux_format(img, &o, err, sizeof err) == -1);
  o.fs = "";
  CHECK(rufux_format(img, &o, err, sizeof err) == -1);

  // Refusing real format without --yes
  o.fs = "vfat";
  o.yes = 0;
  CHECK(rufux_format(img, &o, err, sizeof err) == -1);
  CHECK(strstr(err, "--yes") != NULL);

  // Refusing regular file without --allow-file
  o.yes = 1;
  o.allow_file = 0;
  CHECK(rufux_format(img, &o, err, sizeof err) == -1);

  // Unconditionally refuse boot disk /dev/disk0 even with allow_fixed=1, yes=1
  o.allow_fixed = 1;
  CHECK(rufux_format("/dev/disk0", &o, err, sizeof err) == -1);
  o.fs = "ext4";
  CHECK(rufux_format_ext_from_dir("/dev/disk0", &o, tmpdir, err, sizeof err) == -1);
  CHECK(rufux_ntfs_populate_dir("/dev/disk0", tmpdir, NULL, NULL, err, sizeof err) == -1);
  CHECK(rufux_ntfs_write_single_file("/dev/disk0", img, "a.bin", NULL, NULL, err, sizeof err) == -1);

  o.allow_fixed = 0;
  o.allow_file = 1;

  // ReFS refusal message
  o.fs = "refs";
  CHECK(rufux_format(img, &o, err, sizeof err) == -1);
  CHECK(strstr(err, "ReFS") != NULL && strstr(err, "ntfs") != NULL);
  o.fs = "refsv1";
  CHECK(rufux_format(img, &o, err, sizeof err) == -1);
  CHECK(strstr(err, "ReFS") != NULL && strstr(err, "ntfs") != NULL);

  // Unknown filesystem refusal
  o.fs = "btrfs";
  CHECK(rufux_format(img, &o, err, sizeof err) == -1);
  CHECK(strstr(err, "unsupported fs") != NULL);

  // Label length limits: vfat/fat16 11, exfat 15, ext2/3/4 16, ntfs/udf 32
  const struct {
    const char *fs;
    size_t max_len;
  } limits[] = {
    {"vfat",  11},
    {"fat32", 11},
    {"fat16", 11},
    {"exfat", 15},
    {"ext2",  16},
    {"ext3",  16},
    {"ext4",  16},
    {"ntfs",  32},
    {"udf",   32},
  };
  for (size_t k = 0; k < sizeof(limits) / sizeof(limits[0]); k++) {
    char ok_lbl[64] = {0};
    char bad_lbl[64] = {0};
    memset(ok_lbl, 'A', limits[k].max_len);
    memset(bad_lbl, 'B', limits[k].max_len + 1);

    o.fs = limits[k].fs;
    o.dry_run = 1;
    o.label = ok_lbl;
    CHECK(rufux_format(img, &o, err, sizeof err) == 0);

    o.label = bad_lbl;
    CHECK(rufux_format(img, &o, err, sizeof err) == -1);
    CHECK(strstr(err, "too long") != NULL);
  }

  // FAT16 > 4 GiB guard
  char fat16_big[PATH_MAX];
  snprintf(fat16_big, sizeof fat16_big, "%s/fat16_over4g.img", tmpdir);
  create_sparse_image(fat16_big, (4ULL << 30) + (1ULL << 20));
  o.fs = "fat16";
  o.label = "FAT16BIG";
  o.dry_run = 0;
  o.yes = 1;
  CHECK(rufux_format(fat16_big, &o, err, sizeof err) == -1);
  CHECK(strstr(err, "4 GiB") != NULL && strstr(err, "FAT32") != NULL);
  unlink(fat16_big);

  // rufux_create_persist guards
  CHECK(rufux_create_persist(tmpdir, "casper-rw", 0, 0, err, sizeof err) == -1);
  CHECK(rufux_create_persist(tmpdir, "casper-rw", 16385, 0, err, sizeof err) == -1);
  CHECK(rufux_create_persist("/tmp/nonexistent-rufux-dir-xyz", "casper-rw", 16, 0, err, sizeof err) == -1);
  CHECK(rufux_create_persist(tmpdir, "12345678901234567", 16, 0, err, sizeof err) == -1);
  CHECK(rufux_create_persist(tmpdir, "../escape", 16, 0, err, sizeof err) == -1);
  CHECK(rufux_create_persist(tmpdir, "dry-persist", 16, 1, err, sizeof err) == 0);
  char dry_p[PATH_MAX];
  snprintf(dry_p, sizeof dry_p, "%s/dry-persist", tmpdir);
  CHECK(access(dry_p, F_OK) != 0);

  unlink(img);
}

static void verify_native_fsck_on_image(const char *img_path, const char *fsck_bin, const char *flag) {
  char disk_node[128] = {0};
  CHECK(rufux_macos_attach_raw_image(img_path, /*readonly=*/1, disk_node, sizeof disk_node) == 0);
  snprintf(g_attached_disk, sizeof g_attached_disk, "%s", disk_node);

  char rdisk[160];
  rufux_macos_raw_node(disk_node, rdisk, sizeof rdisk);
  char out[4096] = {0};
  const char *av[] = {fsck_bin, flag, rdisk, NULL};
  int rc = rufux_capture(av, out, sizeof out);
  if (rc != 0) {
    fprintf(stderr, "fsck failed (%s %s %s): %s\n", fsck_bin, flag, rdisk, out);
  }
  CHECK(rc == 0);

  CHECK(rufux_macos_detach_disk(disk_node) == 0);
  g_attached_disk[0] = 0;
}

static void verify_e2fsck_clean(const char *path) {
  char out[4096] = {0};
  const char *av[] = {"e2fsck", "-fn", path, NULL};
  int rc = rufux_capture(av, out, sizeof out);
  if (rc != 0) {
    fprintf(stderr, "e2fsck -fn failed on %s:\n%s\n", path, out);
  }
  CHECK(rc == 0);
}

static void test_format_all_8_filesystems(const char *tmpdir) {
  const unsigned long long img_sz = 64ULL * 1024ULL * 1024ULL; // 64 MiB
  char img[PATH_MAX], err[512];
  unsigned char bs[2048];

  // 1. vfat (FAT32: 64 MiB default cluster size + 128 MiB explicit -c 2 -> >=65525 clusters)
  snprintf(img, sizeof img, "%s/fs_vfat.img", tmpdir);
  create_sparse_image(img, 128ULL * 1024ULL * 1024ULL);
  RufuxMkfsOpts o = {
    .fs = "vfat", .label = "RUFUX_FAT32", .cluster_sectors = 2,
    .dry_run = 0, .allow_fixed = 0, .allow_file = 1, .yes = 1
  };
  CHECK(rufux_format(img, &o, err, sizeof err) == 0);
  read_at_exact(img, bs, 512, 0);
  CHECK(bs[0x1FE] == 0x55u && bs[0x1FF] == 0xAAu);
  CHECK(bs[0x0D] == 2u);
  CHECK(memcmp(bs + 0x47, "RUFUX_FAT32", 11) == 0);
  CHECK(memcmp(bs + 0x52, "FAT32   ", 8) == 0);
  verify_native_fsck_on_image(img, "/sbin/fsck_msdos", "-n");
  unlink(img);

  // 2. fat16
  snprintf(img, sizeof img, "%s/fs_fat16.img", tmpdir);
  create_sparse_image(img, img_sz);
  o.fs = "fat16";
  o.label = "RUFUX_FAT16";
  o.cluster_sectors = 16;
  CHECK(rufux_format(img, &o, err, sizeof err) == 0);
  read_at_exact(img, bs, 512, 0);
  CHECK(bs[0x1FE] == 0x55u && bs[0x1FF] == 0xAAu);
  CHECK(bs[0x0D] == 16u);
  CHECK(memcmp(bs + 0x2B, "RUFUX_FAT16", 11) == 0);
  CHECK(memcmp(bs + 0x36, "FAT16   ", 8) == 0);
  verify_native_fsck_on_image(img, "/sbin/fsck_msdos", "-n");
  unlink(img);

  // 3. exfat (both <=11-char and 15-char labels + custom cluster_sectors)
  snprintf(img, sizeof img, "%s/fs_exfat.img", tmpdir);
  create_sparse_image(img, img_sz);
  o.fs = "exfat";
  o.label = "RUFUX_EXFAT";
  o.cluster_sectors = 32; // 2^5 sectors per cluster
  CHECK(rufux_format(img, &o, err, sizeof err) == 0);
  read_at_exact(img, bs, 512, 0);
  CHECK(memcmp(bs + 3, "EXFAT   ", 8) == 0);
  CHECK(bs[0x1FE] == 0x55u && bs[0x1FF] == 0xAAu);
  CHECK(bs[0x6D] == 5u); // SectorsPerClusterShift: 2^5 = 32
  verify_native_fsck_on_image(img, "/sbin/fsck_exfat", "-n");

  // Re-format same image with 15-char label and default cluster size (-R behavior)
  o.label = "EXFAT_15_CHARS_";
  o.cluster_sectors = 0;
  CHECK(rufux_format(img, &o, err, sizeof err) == 0);
  verify_native_fsck_on_image(img, "/sbin/fsck_exfat", "-n");
  unlink(img);

  // 4. udf
  snprintf(img, sizeof img, "%s/fs_udf.img", tmpdir);
  create_sparse_image(img, img_sz);
  o.fs = "udf";
  o.label = "RUFUX_UDF_VOLUME_LABEL_32_CHARS!";
  o.cluster_sectors = 0;
  CHECK(rufux_format(img, &o, err, sizeof err) == 0);
  unsigned char vrs[64] = {0};
  read_at_exact(img, vrs, 16, 32768ULL);
  CHECK(memcmp(vrs + 1, "BEA01", 5) == 0);
  verify_native_fsck_on_image(img, "/sbin/fsck_udf", "-p");
  unlink(img);

  // 5. ntfs (on regular file + on live GPT partition slice /dev/rdiskKs1)
  snprintf(img, sizeof img, "%s/fs_ntfs.img", tmpdir);
  create_sparse_image(img, img_sz);
  o.fs = "ntfs";
  o.label = "RUFUX_NTFS_LABEL_UP_TO_32_CHARS!";
  o.cluster_sectors = 8; // 4096 bytes
  CHECK(rufux_format(img, &o, err, sizeof err) == 0);
  read_at_exact(img, bs, 512, 0);
  CHECK(memcmp(bs + 3, "NTFS    ", 8) == 0);
  CHECK(bs[0x1FE] == 0x55u && bs[0x1FF] == 0xAAu);
  CHECK(bs[0x0D] == 8u);
  CHECK(rd_le16(bs + 0x18) == 63u);  // -S 63
  CHECK(rd_le16(bs + 0x1A) == 255u); // -H 255
  CHECK(rd_le64(bs + 0x28) <= (img_sz / 512ULL));
  ntfs_volume *nvol = ntfs_mount(img, NTFS_MNT_RDONLY);
  CHECK(nvol != NULL);
  CHECK(nvol->vol_name != NULL &&
        strcmp(nvol->vol_name, "RUFUX_NTFS_LABEL_UP_TO_32_CHARS!") == 0);
  CHECK(ntfs_umount(nvol, FALSE) == 0);
  unlink(img);

  // 5b. ntfs on live GPT partition slice (/dev/rdiskKs1 -> buffered /dev/diskKs1 + -p 2048)
  char gpt_img[PATH_MAX];
  snprintf(gpt_img, sizeof gpt_img, "%s/gpt_ntfs_128m.img", tmpdir);
  create_sparse_image(gpt_img, 128ULL * 1024ULL * 1024ULL);
  RufuxPartOpts po = {
    .scheme = "gpt", .layout = "single", .fs_main = "ntfs", .bios_boot = 0,
    .dry_run = 0, .allow_fixed = 0, .allow_file = 1, .yes = 1
  };
  CHECK(rufux_partition(gpt_img, &po, err, sizeof err) == 0);

  char disk_node[128] = {0};
  CHECK(rufux_macos_attach_raw_image(gpt_img, /*readonly=*/0, disk_node, sizeof disk_node) == 0);
  snprintf(g_attached_disk, sizeof g_attached_disk, "%s", disk_node);
  char s1[160], rs1[160];
  rufux_macos_part_slice(disk_node, 1, s1, sizeof s1);
  rufux_macos_raw_node(s1, rs1, sizeof rs1);
  for (int w = 0; w < 100 && access(s1, F_OK) != 0; w++) usleep(50000);
  CHECK(access(s1, F_OK) == 0);

  RufuxMkfsOpts slice_mo = {
    .fs = "ntfs", .label = "SLICE_NTFS", .cluster_sectors = 0,
    .dry_run = 0, .allow_fixed = 1, .allow_file = 0, .yes = 1
  };
  // Pass raw /dev/rdiskKs1 to confirm rufux_format AND rufux_ntfs_write_single_file
  // route mkntfs, libntfs-3g, and ntfsfix through buffered /dev/diskKs1!
  CHECK(rufux_format(rs1, &slice_mo, err, sizeof err) == 0);
  char slice_src[PATH_MAX];
  snprintf(slice_src, sizeof slice_src, "%s/slice_probe.txt", tmpdir);
  write_text_file(slice_src, "hello from live /dev/diskKs1 slice!");
  CHECK(rufux_ntfs_write_single_file(rs1, slice_src, "EFI/Boot/probe.txt",
                                     NULL, NULL, err, sizeof err) == 0);
  unlink(slice_src);
  CHECK(rufux_macos_detach_disk(disk_node) == 0);
  g_attached_disk[0] = 0;

  read_at_exact(gpt_img, bs, 512, 2048ULL * 512ULL);
  CHECK(memcmp(bs + 3, "NTFS    ", 8) == 0);
  CHECK(rd_le32(bs + 0x1C) == 2048u); // hidden_sectors == partition start LBA 2048!
  CHECK(rd_le16(bs + 0x18) == 63u);
  CHECK(rd_le16(bs + 0x1A) == 255u);
  unlink(gpt_img);

  // 6, 7, 8. ext2, ext3, ext4 (verifying distinct journal/extent superblock features!)
  const struct {
    const char *fs;
    const char *label;
    int want_journal;
    int want_extents;
  } exts[] = {
    {"ext2", "RUFUX_EXT2_16CHR", 0, 0},
    {"ext3", "RUFUX_EXT3_16CHR", 1, 0},
    {"ext4", "RUFUX_EXT4_16CHR", 1, 1},
  };
  for (size_t k = 0; k < 3; k++) {
    snprintf(img, sizeof img, "%s/fs_%s.img", tmpdir, exts[k].fs);
    create_sparse_image(img, img_sz);
    o.fs = exts[k].fs;
    o.label = exts[k].label;
    o.cluster_sectors = 0;
    CHECK(rufux_format(img, &o, err, sizeof err) == 0);

    unsigned char sb[1024] = {0};
    read_at_exact(img, sb, sizeof sb, 1024ULL);
    CHECK(rd_le16(sb + 0x38) == 0xEF53u); // s_magic at 1024 + 56 = 1080
    uint32_t compat = rd_le32(sb + 0x5C);   // s_feature_compat
    uint32_t incompat = rd_le32(sb + 0x60); // s_feature_incompat
    int has_journal = (compat & 0x0004u) != 0;   // EXT3_FEATURE_COMPAT_HAS_JOURNAL
    int has_extents = (incompat & 0x0040u) != 0; // EXT4_FEATURE_INCOMPAT_EXTENTS
    CHECK(has_journal == exts[k].want_journal);
    CHECK(has_extents == exts[k].want_extents);
    CHECK(memcmp(sb + 0x78, exts[k].label, 16) == 0); // s_volume_name[16]

    verify_e2fsck_clean(img);
    unlink(img);
  }
}

static void test_ext_from_dir_and_persist(const char *tmpdir) {
  char stage[PATH_MAX], grub_dir[PATH_MAX], cfg_path[PATH_MAX], readme_path[PATH_MAX], link_path[PATH_MAX];
  snprintf(stage, sizeof stage, "%s/ext_stage", tmpdir);
  snprintf(grub_dir, sizeof grub_dir, "%s/boot/grub", stage);
  snprintf(cfg_path, sizeof cfg_path, "%s/boot/grub/grub.cfg", stage);
  snprintf(readme_path, sizeof readme_path, "%s/README.txt", stage);
  snprintf(link_path, sizeof link_path, "%s/boot/latest.cfg", stage);

  mkdir_p(grub_dir);
  const char *cfg_body = "set timeout=5\nmenuentry 'Rufux Live' { linux /casper/vmlinuz persistent }\n";
  write_text_file(cfg_path, cfg_body);
  write_text_file(readme_path, "Rufux macOS userspace e2fsprogs population test\n");
  CHECK(symlink("grub/grub.cfg", link_path) == 0);

  char err[512] = {0};
  // Create 16 MiB casper-rw persistence file inside stage/
  CHECK(rufux_create_persist(stage, "casper-rw", 16, /*dry_run=*/0, err, sizeof err) == 0);

  char persist_path[PATH_MAX];
  snprintf(persist_path, sizeof persist_path, "%s/casper-rw", stage);
  struct stat pst;
  CHECK(stat(persist_path, &pst) == 0);
  CHECK((unsigned long long)pst.st_size == 16ULL * 1024ULL * 1024ULL);
  unsigned char psb[1024] = {0};
  read_at_exact(persist_path, psb, sizeof psb, 1024ULL);
  CHECK(rd_le16(psb + 0x38) == 0xEF53u);
  CHECK(strncmp((const char *)(psb + 0x78), "casper-rw", 9) == 0);
  verify_e2fsck_clean(persist_path);

  const char *ext_flavors[] = {"ext2", "ext3", "ext4"};
  for (int i = 0; i < 3; i++) {
    char img[PATH_MAX];
    snprintf(img, sizeof img, "%s/populated_%s.img", tmpdir, ext_flavors[i]);
    create_sparse_image(img, 64ULL * 1024ULL * 1024ULL);

    char lbl[32];
    snprintf(lbl, sizeof lbl, "LIVE_%s", ext_flavors[i]);
    RufuxMkfsOpts mo = {
      .fs = ext_flavors[i],
      .label = lbl,
      .cluster_sectors = 0,
      .dry_run = 0,
      .allow_fixed = 0,
      .allow_file = 1,
      .yes = 1
    };
    CHECK(rufux_format_ext_from_dir(img, &mo, stage, err, sizeof err) == 0);
    verify_e2fsck_clean(img);

    // Inspect populated contents via debugfs to prove nested files, symlink, and casper-rw exist!
    char out[4096] = {0};
    const char *cat_av[] = {"debugfs", "-R", "cat /boot/grub/grub.cfg", img, NULL};
    CHECK(rufux_capture(cat_av, out, sizeof out) == 0);
    CHECK(strstr(out, "menuentry 'Rufux Live'") != NULL);

    const char *lnk_av[] = {"debugfs", "-R", "stat /boot/latest.cfg", img, NULL};
    CHECK(rufux_capture(lnk_av, out, sizeof out) == 0);
    CHECK(strstr(out, "grub/grub.cfg") != NULL);

    const char *prw_av[] = {"debugfs", "-R", "stat /casper-rw", img, NULL};
    CHECK(rufux_capture(prw_av, out, sizeof out) == 0);
    CHECK(strstr(out, "16777216") != NULL);

    unlink(img);
  }
}

typedef struct {
  int calls;
  unsigned long long last_done;
  unsigned long long last_total;
  int monotonic;
} NtfsProgTrack;

static void track_ntfs_prog(unsigned long long done, unsigned long long total, void *u) {
  NtfsProgTrack *t = (NtfsProgTrack *)u;
  if (t->calls > 0 && done < t->last_done) t->monotonic = 0;
  if (total > 0 && done > total) t->monotonic = 0;
  t->last_done = done;
  t->last_total = total;
  t->calls++;
}

typedef struct {
  int count;
  char names[16][128];
} DirListCtx;

static int collect_ntfs_dirent(void *dirent, const ntfschar *name,
                               const int name_len, const int name_type,
                               const s64 pos, const MFT_REF mref,
                               const unsigned dt_type) {
  (void)pos;
  (void)dt_type;
  if (name_type == FILE_NAME_DOS) return 0;
  if (MREF(mref) < FILE_first_user && MREF(mref) != FILE_root) return 0;

  char *utf8 = NULL;
  int n = ntfs_ucstombs(name, name_len, &utf8, 0);
  if (n < 0 || !utf8) return 0;
  if (strcmp(utf8, ".") != 0 && strcmp(utf8, "..") != 0) {
    DirListCtx *ctx = (DirListCtx *)dirent;
    if (ctx->count < 16) {
      snprintf(ctx->names[ctx->count], sizeof ctx->names[0], "%s", utf8);
    }
    ctx->count++;
  }
  free(utf8);
  return 0;
}

static void fill_64bit_pattern(unsigned char *buf, size_t len, unsigned long long base_off) {
  for (size_t i = 0; i < len; i++) {
    uint64_t x = base_off + (uint64_t)i;
    x ^= (x >> 33);
    x *= 0xff51afd7ed558ccdULL;
    x ^= (x >> 33);
    unsigned char b = (unsigned char)((x & 0xFEu) | 1u); // guaranteed non-zero
    buf[i] = b;
  }
}

static void read_ntfs_file_exact(ntfs_volume *vol, const char *path, void *out, size_t want_len) {
  ntfs_inode *ni = ntfs_pathname_to_inode(vol, NULL, path);
  CHECK(ni != NULL);
  ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
  CHECK(na != NULL);
  CHECK(na->data_size == (s64)want_len);
  s64 r = ntfs_attr_pread(na, 0, (s64)want_len, out);
  CHECK(r == (s64)want_len);
  ntfs_attr_close(na);
  CHECK(ntfs_inode_close(ni) == 0);
}

static void test_ntfs_populate_case_merge_and_64bit_offset(const char *tmpdir) {
  // 5 GiB sparse NTFS image
  const unsigned long long vol_bytes = 5ULL * 1024ULL * 1024ULL * 1024ULL;
  char ntfs_img[PATH_MAX];
  snprintf(ntfs_img, sizeof ntfs_img, "%s/ntfs_5gib.img", tmpdir);
  create_sparse_image(ntfs_img, vol_bytes);

  char err[512] = {0};
  RufuxMkfsOpts mo = {
    .fs = "ntfs",
    .label = "WIN11_64BIT",
    .cluster_sectors = 0,
    .dry_run = 0,
    .allow_fixed = 0,
    .allow_file = 1,
    .yes = 1
  };
  CHECK(rufux_format(ntfs_img, &mo, err, sizeof err) == 0);

  // Part A: Populate tree_a ("EFI/Boot/bootx64.efi") then tree_b ("efi/boot/grubx64.efi")
  // plus single-file write ("EfI/bOoT/mmx64.efi") and verify they merge into ONE EFI/Boot dir!
  char tree_a[PATH_MAX], tree_b[PATH_MAX], dir_a[PATH_MAX], dir_b[PATH_MAX];
  char f_bootx64[PATH_MAX], f_grubx64[PATH_MAX], f_mmx64[PATH_MAX];
  snprintf(tree_a, sizeof tree_a, "%s/tree_a", tmpdir);
  snprintf(tree_b, sizeof tree_b, "%s/tree_b", tmpdir);
  snprintf(dir_a, sizeof dir_a, "%s/EFI/Boot", tree_a);
  snprintf(dir_b, sizeof dir_b, "%s/efi/boot", tree_b);
  snprintf(f_bootx64, sizeof f_bootx64, "%s/bootx64.efi", dir_a);
  snprintf(f_grubx64, sizeof f_grubx64, "%s/grubx64.efi", dir_b);
  snprintf(f_mmx64, sizeof f_mmx64, "%s/mmx64_src.efi", tmpdir);

  mkdir_p(dir_a);
  mkdir_p(dir_b);
  const char *boot_payload = "MZ_EFI_BOOTX64_PAYLOAD_1234567890";
  const char *grub_payload = "MZ_EFI_GRUBX64_PAYLOAD_0987654321";
  const char *mm_payload   = "MZ_EFI_MMX64_PAYLOAD_SINGLE_FILE!";
  write_text_file(f_bootx64, boot_payload);
  write_text_file(f_grubx64, grub_payload);
  write_text_file(f_mmx64, mm_payload);

  NtfsProgTrack pt = {.calls = 0, .last_done = 0, .last_total = 0, .monotonic = 1};
  CHECK(rufux_ntfs_populate_dir(ntfs_img, tree_a, track_ntfs_prog, &pt, err, sizeof err) == 0);
  CHECK(pt.calls > 0 && pt.monotonic == 1 && pt.last_done == strlen(boot_payload));

  pt = (NtfsProgTrack){.calls = 0, .last_done = 0, .last_total = 0, .monotonic = 1};
  CHECK(rufux_ntfs_populate_dir(ntfs_img, tree_b, track_ntfs_prog, &pt, err, sizeof err) == 0);
  CHECK(pt.calls > 0 && pt.monotonic == 1 && pt.last_done == strlen(grub_payload));

  pt = (NtfsProgTrack){.calls = 0, .last_done = 0, .last_total = 0, .monotonic = 1};
  CHECK(rufux_ntfs_write_single_file(ntfs_img, f_mmx64, "EfI/bOoT/mmx64.efi",
                                     track_ntfs_prog, &pt, err, sizeof err) == 0);
  CHECK(pt.calls > 0 && pt.monotonic == 1 && pt.last_done == strlen(mm_payload));

  // Verify on-disk NTFS directory structure has strictly ONE "EFI" and ONE "Boot"
  ntfs_volume *vol = ntfs_mount(ntfs_img, NTFS_MNT_RDONLY);
  CHECK(vol != NULL);

  ntfs_inode *root_ni = ntfs_inode_open(vol, FILE_root);
  CHECK(root_ni != NULL);
  DirListCtx root_list = {0};
  s64 pos = 0;
  CHECK(ntfs_readdir(root_ni, &pos, &root_list, collect_ntfs_dirent) == 0);
  CHECK(ntfs_inode_close(root_ni) == 0);
  CHECK(root_list.count == 1);
  CHECK(strcmp(root_list.names[0], "EFI") == 0);

  ntfs_inode *efi_ni = ntfs_pathname_to_inode(vol, NULL, "EFI");
  CHECK(efi_ni != NULL);
  DirListCtx efi_list = {0};
  pos = 0;
  CHECK(ntfs_readdir(efi_ni, &pos, &efi_list, collect_ntfs_dirent) == 0);
  CHECK(ntfs_inode_close(efi_ni) == 0);
  CHECK(efi_list.count == 1);
  CHECK(strcmp(efi_list.names[0], "Boot") == 0);

  ntfs_inode *boot_ni = ntfs_pathname_to_inode(vol, NULL, "EFI/Boot");
  CHECK(boot_ni != NULL);
  DirListCtx boot_list = {0};
  pos = 0;
  CHECK(ntfs_readdir(boot_ni, &pos, &boot_list, collect_ntfs_dirent) == 0);
  CHECK(ntfs_inode_close(boot_ni) == 0);
  CHECK(boot_list.count == 3);

  char rbuf[128] = {0};
  read_ntfs_file_exact(vol, "EFI/Boot/bootx64.efi", rbuf, strlen(boot_payload));
  CHECK(memcmp(rbuf, boot_payload, strlen(boot_payload)) == 0);

  memset(rbuf, 0, sizeof rbuf);
  read_ntfs_file_exact(vol, "EFI/Boot/grubx64.efi", rbuf, strlen(grub_payload));
  CHECK(memcmp(rbuf, grub_payload, strlen(grub_payload)) == 0);

  memset(rbuf, 0, sizeof rbuf);
  read_ntfs_file_exact(vol, "EFI/Boot/mmx64.efi", rbuf, strlen(mm_payload));
  CHECK(memcmp(rbuf, mm_payload, strlen(mm_payload)) == 0);

  CHECK(ntfs_umount(vol, FALSE) == 0);

  // Part B: Stream a 4.1 GiB sparse/patterned file via rufux_ntfs_write_single_file
  // and verify bytes across 2 GiB, 4 GiB (0x100000000ULL), 4 GiB + 4 KiB, and tail!
  const unsigned long long big_size =
      (4ULL << 30) + (103ULL << 20); // ~4.1006 GiB (4,402,970,624 bytes)
  const unsigned long long off_head = 0ULL;
  const unsigned long long off_2g   = 0x7FFF0000ULL; // spans 0x7FFF0000..0x80010000
  const unsigned long long off_4g   = 0xFFFF0000ULL; // spans 0xFFFF0000..0x100010000 (past 4 GiB + 4 KiB!)
  const unsigned long long off_tail = big_size - 65536ULL;

  char big_src[PATH_MAX];
  snprintf(big_src, sizeof big_src, "%s/install_4p1g.wim", tmpdir);
  int bfd = open(big_src, O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0600);
  CHECK(bfd >= 0);
  CHECK(ftruncate(bfd, (off_t)big_size) == 0);

  unsigned char pat64k[65536];
  unsigned char pat128k[131072];

  fill_64bit_pattern(pat64k, sizeof pat64k, off_head);
  CHECK(rufux_pwrite_all(bfd, pat64k, sizeof pat64k, (off_t)off_head) == 0);

  fill_64bit_pattern(pat128k, sizeof pat128k, off_2g);
  CHECK(rufux_pwrite_all(bfd, pat128k, sizeof pat128k, (off_t)off_2g) == 0);

  fill_64bit_pattern(pat128k, sizeof pat128k, off_4g);
  CHECK(rufux_pwrite_all(bfd, pat128k, sizeof pat128k, (off_t)off_4g) == 0);

  fill_64bit_pattern(pat64k, sizeof pat64k, off_tail);
  CHECK(rufux_pwrite_all(bfd, pat64k, sizeof pat64k, (off_t)off_tail) == 0);
  CHECK(close(bfd) == 0);

  pt = (NtfsProgTrack){.calls = 0, .last_done = 0, .last_total = 0, .monotonic = 1};
  CHECK(rufux_ntfs_write_single_file(ntfs_img, big_src, "sources/install.wim",
                                     track_ntfs_prog, &pt, err, sizeof err) == 0);
  unlink(big_src);
  CHECK(pt.calls >= 100);
  CHECK(pt.monotonic == 1);
  CHECK(pt.last_done == big_size && pt.last_total == big_size);

  // Read back directly via libntfs-3g at 64-bit offsets > 4 GiB!
  vol = ntfs_mount(ntfs_img, NTFS_MNT_RDONLY);
  CHECK(vol != NULL);
  ntfs_inode *wim_ni = ntfs_pathname_to_inode(vol, NULL, "sources/install.wim");
  CHECK(wim_ni != NULL);
  ntfs_attr *wim_na = ntfs_attr_open(wim_ni, AT_DATA, AT_UNNAMED, 0);
  CHECK(wim_na != NULL);
  CHECK(wim_na->data_size == (s64)big_size);

  unsigned char got128k[131072];
  unsigned char got64k[65536];

  // 1. Head (offset 0)
  fill_64bit_pattern(pat64k, sizeof pat64k, off_head);
  CHECK(ntfs_attr_pread(wim_na, (s64)off_head, (s64)sizeof got64k, got64k) == (s64)sizeof got64k);
  CHECK(memcmp(got64k, pat64k, sizeof pat64k) == 0);

  // 2. Sparse zero middle (offset 1 GiB)
  CHECK(ntfs_attr_pread(wim_na, (s64)(1ULL << 30), (s64)sizeof got64k, got64k) == (s64)sizeof got64k);
  for (size_t i = 0; i < sizeof got64k; i++) CHECK(got64k[i] == 0u);

  // 3. Across signed 32-bit boundary 2 GiB (0x7FFF0000..0x80010000)
  fill_64bit_pattern(pat128k, sizeof pat128k, off_2g);
  CHECK(ntfs_attr_pread(wim_na, (s64)off_2g, (s64)sizeof got128k, got128k) == (s64)sizeof got128k);
  CHECK(memcmp(got128k, pat128k, sizeof pat128k) == 0);

  // 4. Across unsigned 32-bit boundary 4 GiB (0xFFFF0000..0x100010000)
  //    and explicit direct pread at 0x100001000ULL (4 GiB + 4 KiB)!
  fill_64bit_pattern(pat128k, sizeof pat128k, off_4g);
  CHECK(ntfs_attr_pread(wim_na, (s64)off_4g, (s64)sizeof got128k, got128k) == (s64)sizeof got128k);
  CHECK(memcmp(got128k, pat128k, sizeof pat128k) == 0);

  const unsigned long long off_4g_plus_4k = 0x100001000ULL;
  unsigned char got4k[4096], want4k[4096];
  fill_64bit_pattern(want4k, sizeof want4k, off_4g_plus_4k);
  CHECK(ntfs_attr_pread(wim_na, (s64)off_4g_plus_4k, (s64)sizeof got4k, got4k) == (s64)sizeof got4k);
  CHECK(memcmp(got4k, want4k, sizeof want4k) == 0);

  // 5. Tail at 4.1 GiB - 64 KiB
  fill_64bit_pattern(pat64k, sizeof pat64k, off_tail);
  CHECK(ntfs_attr_pread(wim_na, (s64)off_tail, (s64)sizeof got64k, got64k) == (s64)sizeof got64k);
  CHECK(memcmp(got64k, pat64k, sizeof pat64k) == 0);

  ntfs_attr_close(wim_na);
  CHECK(ntfs_inode_close(wim_ni) == 0);
  CHECK(ntfs_umount(vol, FALSE) == 0);

  // Confirm ntfsfix -n finds zero inconsistencies on the 5 GiB volume
  char fix_out[4096] = {0};
  const char *fix_av[] = {"ntfsfix", "-n", ntfs_img, NULL};
  CHECK(rufux_capture(fix_av, fix_out, sizeof fix_out) == 0);

  // Reclaim the 5 GiB image immediately
  unlink(ntfs_img);
}

int main(void) {
  atexit(emergency_cleanup);
  rufux_macos_init_bundle_env();

  char tmpdir[PATH_MAX];
  make_tmpdir(tmpdir, sizeof tmpdir);

  test_fs_helpers_and_guards(tmpdir);
  test_format_all_8_filesystems(tmpdir);
  test_ext_from_dir_and_persist(tmpdir);
  test_ntfs_populate_case_merge_and_64bit_offset(tmpdir);

  emergency_cleanup();
  printf("ctest_filesystems: ALL TESTS PASSED\n");
  return 0;
}

#include "mkfs_task.h"
#include "device.h"
#include "exec.h"
#include "partition.h"
#include "platform_macos.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

__attribute__((format(printf, 3, 4)))
static void set_err(char *err, unsigned long cap, const char *fmt, ...) {
  if (!err || cap == 0) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, cap, fmt, ap);
  va_end(ap);
}

int rufux_fs_is_fat16(const char *fs) {
  return fs && strcmp(fs, "fat16") == 0;
}

int rufux_fs_is_fat(const char *fs) {
  return fs && (strcmp(fs, "vfat") == 0 ||
                strcmp(fs, "fat32") == 0 ||
                strcmp(fs, "fat16") == 0);
}

static int is_ext_fs(const char *fs) {
  return fs && (strcmp(fs, "ext2") == 0 ||
                strcmp(fs, "ext3") == 0 ||
                strcmp(fs, "ext4") == 0);
}

// Parses /dev/[r]disk<K> or /dev/[r]disk<K>s<M>.
// Returns 1 for a partition slice (setting parent_disk="/dev/disk<K>" and *slice_out=M),
// 2 for a whole disk (setting parent_disk="/dev/disk<K>" and *slice_out=0), or 0 otherwise.
static int parse_bsd_target(const char *path, char *parent_disk, size_t pcap,
                            int *slice_out, char *bsd_out, size_t bcap) {
  if (parent_disk && pcap > 0) parent_disk[0] = 0;
  if (slice_out) *slice_out = 0;
  if (bsd_out && bcap > 0) bsd_out[0] = 0;
  if (!path || !path[0]) return 0;

  char resolved[PATH_MAX];
  const char *p = realpath(path, resolved) ? resolved : path;
  if (strncmp(p, "/private/dev/", 13) == 0) p += 8;

  const char *cand = NULL;
  if (strncmp(p, "/dev/rdisk", 10) == 0) cand = p + 6;
  else if (strncmp(p, "/dev/disk", 9) == 0) cand = p + 5;
  else return 0;

  if (strncmp(cand, "disk", 4) != 0 || !isdigit((unsigned char)cand[4])) return 0;
  size_t i = 4;
  while (isdigit((unsigned char)cand[i])) i++;

  char whole_bsd[64];
  if (i >= sizeof whole_bsd) return 0;
  memcpy(whole_bsd, cand, i);
  whole_bsd[i] = 0;

  if (cand[i] == '\0') {
    if (parent_disk && pcap > 0) snprintf(parent_disk, pcap, "/dev/%s", whole_bsd);
    if (bsd_out && bcap > 0) snprintf(bsd_out, bcap, "%s", cand);
    return 2;
  }

  if (cand[i] != 's' || !isdigit((unsigned char)cand[i + 1])) return 0;
  char *endp = NULL;
  unsigned long sl = strtoul(cand + i + 1, &endp, 10);
  if (!endp || (*endp != '\0' && *endp != 's') || sl == 0 || sl > 1024) return 0;

  if (parent_disk && pcap > 0) snprintf(parent_disk, pcap, "/dev/%s", whole_bsd);
  if (slice_out) *slice_out = (int)sl;
  if (bsd_out && bcap > 0) snprintf(bsd_out, bcap, "%s", cand);
  return 1;
}

static void unmount_if_mounted_target(const char *dev_path) {
  char parent_disk[128], target_bsd[64];
  int slice = 0;
  int kind = parse_bsd_target(dev_path, parent_disk, sizeof parent_disk,
                              &slice, target_bsd, sizeof target_bsd);
  if (kind == 0) return;

  struct statfs *mnts = NULL;
  int n = getmntinfo(&mnts, MNT_NOWAIT);
  if (n <= 0 || !mnts) return;

  int hit = 0;
  size_t tlen = strlen(target_bsd);
  for (int i = 0; i < n; i++) {
    char mp[128], mbsd[64];
    int ms = 0;
    if (!parse_bsd_target(mnts[i].f_mntfromname, mp, sizeof mp, &ms, mbsd, sizeof mbsd))
      continue;
    if (kind == 1) {
      if (strcmp(mbsd, target_bsd) == 0) { hit = 1; break; }
    } else {
      if (strncmp(mbsd, target_bsd, tlen) == 0 &&
          (mbsd[tlen] == '\0' || mbsd[tlen] == 's')) {
        hit = 1;
        break;
      }
    }
  }
  if (!hit) return;

  char bnode[160], out[512];
  rufux_macos_block_node(dev_path, bnode, sizeof bnode);
  if (kind == 1) {
    const char *av[] = {"/usr/sbin/diskutil", "unmount", "force", bnode, NULL};
    (void)rufux_capture(av, out, sizeof out);
  } else {
    (void)rufux_macos_unmount_disk(bnode, out, sizeof out);
  }
}

// Apple's /sbin/newfs_exfat enforces the on-disk exFAT VolumeLabel[11] limit
// (11 UTF-16 code units), whereas Rufux's pre-flight limit for exfat is 15 chars.
// Truncate on a UTF-8 codepoint boundary so newfs_exfat never rejects 12..15-char labels.
static void truncate_utf8_to_utf16_units(const char *src, size_t max_u16,
                                         char *out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = 0;
  if (!src) return;

  size_t si = 0, oi = 0, u16 = 0;
  while (src[si] != '\0') {
    unsigned char c = (unsigned char)src[si];
    size_t clen = 1;
    size_t cu16 = 1;
    if ((c & 0x80u) == 0) {
      clen = 1;
      cu16 = 1;
    } else if ((c & 0xE0u) == 0xC0u) {
      clen = 2;
      cu16 = 1;
    } else if ((c & 0xF0u) == 0xE0u) {
      clen = 3;
      cu16 = 1;
    } else if ((c & 0xF8u) == 0xF0u) {
      clen = 4;
      cu16 = 2;
    }
    for (size_t k = 1; k < clen; k++) {
      if (src[si + k] == '\0') { clen = k; break; }
    }
    if (u16 + cu16 > max_u16 || oi + clen >= cap) break;
    memcpy(out + oi, src + si, clen);
    oi += clen;
    si += clen;
    u16 += cu16;
  }
  out[oi] = 0;
}

static size_t label_max_for_fs(const char *fs) {
  if (rufux_fs_is_fat(fs)) return 11;
  if (strcmp(fs, "exfat") == 0) return 15;
  if (is_ext_fs(fs)) return 16;
  return 32;
}

static int format_ext_common(const char *dst, const RufuxMkfsOpts *o,
                             const char *root_dir, char *err, unsigned long cap) {
  if (!rufux_have("mkfs.ext4")) {
    set_err(err, cap, "mkfs.ext4 missing");
    return -1;
  }

  char bnode[PATH_MAX];
  rufux_macos_block_node(dst, bnode, sizeof bnode);
  if (rufux_macos_is_boot_disk(bnode)) {
    set_err(err, cap, "refusing boot/system disk '%s'", dst);
    return -1;
  }

  struct stat st;
  if (!o->dry_run && stat(bnode, &st) == 0 && (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode))) {
    unmount_if_mounted_target(bnode);
  }

  char lab_ext[160];
  const char *av[11];
  int i = 0;
  av[i++] = "mkfs.ext4";
  av[i++] = "-t";
  av[i++] = o->fs;
  av[i++] = "-F";
  if (o->label && o->label[0]) {
    snprintf(lab_ext, sizeof lab_ext, "%s", o->label);
    av[i++] = "-L";
    av[i++] = lab_ext;
  }
  if (root_dir && root_dir[0]) {
    av[i++] = "-d";
    av[i++] = root_dir;
  }
  av[i++] = bnode;
  av[i] = NULL;

  if (rufux_run(av, o->dry_run) != 0) {
    if (!o->dry_run) set_err(err, cap, "mkfs.%s failed on '%s'", o->fs, dst);
    return o->dry_run ? 0 : -1;
  }
  return 0;
}

int rufux_format_ext_from_dir(const char *dst, const RufuxMkfsOpts *o,
                              const char *root_dir, char *err, unsigned long cap) {
  if (err && cap > 0) err[0] = 0;
  if (!o || !dst || !dst[0]) {
    set_err(err, cap, "invalid format arguments");
    return -1;
  }
  if (rufux_macos_is_boot_disk(dst)) {
    set_err(err, cap, "refusing boot/system disk '%s'", dst);
    return -1;
  }
  if (!o->fs || !o->fs[0]) {
    set_err(err, cap, "need --fs");
    return -1;
  }
  if (!is_ext_fs(o->fs)) {
    set_err(err, cap, "rufux_format_ext_from_dir requires ext2|ext3|ext4 (got '%s')", o->fs);
    return -1;
  }
  if (!o->dry_run && !o->yes) {
    set_err(err, cap, "refusing real format without --yes");
    return -1;
  }
  if (o->label && o->label[0] && strlen(o->label) > 16) {
    set_err(err, cap, "label '%s' too long for %s (max 16 chars)", o->label, o->fs);
    return -1;
  }
  struct stat rst;
  if (!root_dir || !root_dir[0] || stat(root_dir, &rst) != 0 || !S_ISDIR(rst.st_mode)) {
    set_err(err, cap, "staging dir '%s' missing", root_dir ? root_dir : "");
    return -1;
  }
  if (rufux_check_target(dst, o->allow_fixed, o->allow_file, err, cap) != 0) return -1;

  return format_ext_common(dst, o, root_dir, err, cap);
}

int rufux_format(const char *dst, const RufuxMkfsOpts *o,
                 char *err, unsigned long cap) {
  if (err && cap > 0) err[0] = 0;
  if (!o || !dst || !dst[0]) {
    set_err(err, cap, "invalid format arguments");
    return -1;
  }
  if (rufux_macos_is_boot_disk(dst)) {
    set_err(err, cap, "refusing boot/system disk '%s'", dst);
    return -1;
  }
  if (!o->fs || !o->fs[0]) {
    set_err(err, cap, "need --fs");
    return -1;
  }
  if (!o->dry_run && !o->yes) {
    set_err(err, cap, "refusing real format without --yes");
    return -1;
  }
  if (strcmp(o->fs, "refs") == 0 || strcmp(o->fs, "refsv1") == 0) {
    set_err(err, cap, "ReFS has no macOS formatter (proprietary filesystem); use ntfs");
    return -1;
  }
  if (!rufux_fs_is_fat(o->fs) &&
      strcmp(o->fs, "exfat") != 0 &&
      strcmp(o->fs, "udf") != 0 &&
      strcmp(o->fs, "ntfs") != 0 &&
      !is_ext_fs(o->fs)) {
    set_err(err, cap, "unsupported fs '%s' (vfat|fat16|ntfs|exfat|ext2|ext3|ext4|udf)", o->fs);
    return -1;
  }

  if (o->label && o->label[0]) {
    size_t max = label_max_for_fs(o->fs);
    if (strlen(o->label) > max) {
      set_err(err, cap, "label '%s' too long for %s (max %zu chars)", o->label, o->fs, max);
      return -1;
    }
  }

  if (rufux_check_target(dst, o->allow_fixed, o->allow_file, err, cap) != 0) return -1;

  if (is_ext_fs(o->fs)) {
    return format_ext_common(dst, o, NULL, err, cap);
  }

  struct stat st;
  if (stat(dst, &st) != 0) {
    set_err(err, cap, "target '%s' does not exist", dst);
    return -1;
  }

  // Native macOS formatters (/sbin/newfs_msdos, /sbin/newfs_exfat, /sbin/newfs_udf)
  // require a /dev/[r]disk* node; attach regular files transparently via hdiutil.
  if (rufux_fs_is_fat(o->fs) || strcmp(o->fs, "exfat") == 0 || strcmp(o->fs, "udf") == 0) {
    int is_fat = rufux_fs_is_fat(o->fs);
    int is_fat16 = rufux_fs_is_fat16(o->fs);
    int is_exfat = strcmp(o->fs, "exfat") == 0;
    const char *tool = is_fat ? "/sbin/newfs_msdos"
                              : (is_exfat ? "/sbin/newfs_exfat" : "/sbin/newfs_udf");
    if (!rufux_have(tool)) {
      set_err(err, cap, "%s missing", tool);
      return -1;
    }

    int spc = o->cluster_sectors;
    if (is_fat16) {
      unsigned long long bytes = rufux_macos_dev_bytes(dst);
      if (bytes > (4ULL << 30)) {
        set_err(err, cap, "FAT16 volumes are limited to 4 GiB ('%s' is %.1f GiB); use FAT32",
                dst, (double)bytes / 1073741824.0);
        return -1;
      }
      if (spc <= 0 && bytes > (2ULL << 30)) spc = 128;
    }

    char attached_disk[128] = {0};
    char target_dev[PATH_MAX];
    if (o->dry_run) {
      snprintf(target_dev, sizeof target_dev, "%s", dst);
    } else if (S_ISREG(st.st_mode)) {
      if (rufux_macos_attach_raw_image(dst, /*readonly=*/0,
                                       attached_disk, sizeof attached_disk) != 0) {
        set_err(err, cap, "failed to attach image '%s' via hdiutil", dst);
        return -1;
      }
      if (rufux_macos_is_boot_disk(attached_disk)) {
        (void)rufux_macos_detach_disk(attached_disk);
        set_err(err, cap, "refusing boot/system disk '%s'", attached_disk);
        return -1;
      }
      rufux_macos_raw_node(attached_disk, target_dev, sizeof target_dev);
    } else {
      unmount_if_mounted_target(dst);
      rufux_macos_raw_node(dst, target_dev, sizeof target_dev);
    }

    char spc_str[32], lab_buf[160];
    const char *av[12];
    int i = 0;
    av[i++] = tool;
    if (is_fat) {
      av[i++] = "-F";
      av[i++] = is_fat16 ? "16" : "32";
      if (spc > 0) {
        snprintf(spc_str, sizeof spc_str, "%d", spc);
        av[i++] = "-c";
        av[i++] = spc_str;
      }
      if (o->label && o->label[0]) {
        snprintf(lab_buf, sizeof lab_buf, "%s", o->label);
        av[i++] = "-v";
        av[i++] = lab_buf;
      }
    } else if (is_exfat) {
      av[i++] = "-R";
      if (spc > 0) {
        snprintf(spc_str, sizeof spc_str, "%d", spc);
        av[i++] = "-c";
        av[i++] = spc_str;
      }
      if (o->label && o->label[0]) {
        truncate_utf8_to_utf16_units(o->label, 11, lab_buf, sizeof lab_buf);
        if (lab_buf[0]) {
          av[i++] = "-v";
          av[i++] = lab_buf;
        }
      }
    } else {
      // udf
      if (o->label && o->label[0]) {
        snprintf(lab_buf, sizeof lab_buf, "%s", o->label);
        av[i++] = "-v";
        av[i++] = lab_buf;
      }
    }
    av[i++] = target_dev;
    av[i] = NULL;

    int rc = rufux_run(av, o->dry_run);
    if (attached_disk[0]) {
      unmount_if_mounted_target(attached_disk);
      if (rufux_macos_detach_disk(attached_disk) != 0 && rc == 0) {
        set_err(err, cap, "failed to detach '%s' after formatting '%s'", attached_disk, dst);
        return -1;
      }
    }
    if (rc != 0) {
      if (!o->dry_run) set_err(err, cap, "mkfs.%s failed on '%s'", o->fs, dst);
      return o->dry_run ? 0 : -1;
    }
    return 0;
  }

  // NTFS via vendored mkntfs ("mkfs.ntfs" -> "mkntfs" in exec.c).
  // Always pass buffered /dev/diskNsM (never /dev/rdiskNsM) because libntfs-3g
  // performs non-512-aligned / 1024-byte / 4096-byte cached I/O.
  if (!rufux_have("mkfs.ntfs")) {
    set_err(err, cap, "mkfs.ntfs missing (ntfsprogs)");
    return -1;
  }

  char bnode[PATH_MAX];
  rufux_macos_block_node(dst, bnode, sizeof bnode);
  if (rufux_macos_is_boot_disk(bnode)) {
    set_err(err, cap, "refusing boot/system disk '%s'", dst);
    return -1;
  }
  if (!o->dry_run && (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode))) {
    unmount_if_mounted_target(bnode);
  }

  unsigned sec_sz = rufux_macos_sector_size(bnode);
  if (sec_sz == 0) sec_sz = 512u;

  unsigned long long start_lba = 0ULL;
  unsigned long long part_sectors = 0ULL;
  char parent_disk[128], bsd_name[64];
  int slice_idx = 0;
  if (parse_bsd_target(bnode, parent_disk, sizeof parent_disk,
                       &slice_idx, bsd_name, sizeof bsd_name) == 1) {
    unsigned long long st_lba = 0ULL, cnt_lba = 0ULL;
    if (rufux_part_offset_sectors(parent_disk, slice_idx, &st_lba, &cnt_lba) == 0 &&
        st_lba > 0ULL) {
      start_lba = st_lba;
      part_sectors = cnt_lba;
    } else {
      start_lba = 2048ULL;
    }
  }

  unsigned long long dev_bytes = rufux_macos_dev_bytes(bnode);
  unsigned long long target_sectors = dev_bytes / (unsigned long long)sec_sz;
  if (target_sectors == 0ULL && part_sectors > 0ULL) {
    target_sectors = part_sectors;
  }

  char sec_str[32], start_ntfs[32], clus_ntfs[32], lab_ntfs[160], count_ntfs[32];
  const char *a_ntfs[20];
  int i = 0;
  a_ntfs[i++] = "mkfs.ntfs";
  a_ntfs[i++] = "-F";
  a_ntfs[i++] = "-Q";
  snprintf(sec_str, sizeof sec_str, "%u", sec_sz);
  a_ntfs[i++] = "-s";
  a_ntfs[i++] = sec_str;
  snprintf(start_ntfs, sizeof start_ntfs, "%llu", start_lba);
  a_ntfs[i++] = "-p";
  a_ntfs[i++] = start_ntfs;
  a_ntfs[i++] = "-H";
  a_ntfs[i++] = "255";
  a_ntfs[i++] = "-S";
  a_ntfs[i++] = "63";
  if (o->cluster_sectors > 0) {
    snprintf(clus_ntfs, sizeof clus_ntfs, "%llu",
             (unsigned long long)o->cluster_sectors * 512ULL);
    a_ntfs[i++] = "-c";
    a_ntfs[i++] = clus_ntfs;
  }
  if (o->label && o->label[0]) {
    snprintf(lab_ntfs, sizeof lab_ntfs, "%s", o->label);
    a_ntfs[i++] = "-L";
    a_ntfs[i++] = lab_ntfs;
  }
  a_ntfs[i++] = bnode;
  if (!o->dry_run) {
    if (target_sectors == 0ULL) {
      set_err(err, cap, "cannot determine sector count for '%s'", dst);
      return -1;
    }
    snprintf(count_ntfs, sizeof count_ntfs, "%llu", target_sectors);
    a_ntfs[i++] = count_ntfs;
  }
  a_ntfs[i] = NULL;

  if (rufux_run(a_ntfs, o->dry_run) != 0) {
    if (!o->dry_run) set_err(err, cap, "mkfs.ntfs failed on '%s'", dst);
    return o->dry_run ? 0 : -1;
  }

  if (!o->dry_run && target_sectors > 0ULL) {
    unsigned char bs[512];
    int fd = open(bnode, O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? pread(fd, bs, sizeof bs, 0) : -1;
    if (fd >= 0) close(fd);
    if (n == (ssize_t)sizeof bs) {
      unsigned long long total = 0ULL;
      for (int k = 7; k >= 0; k--) total = (total << 8) | bs[0x28 + k];
      if (total > target_sectors) {
        set_err(err, cap,
                "NTFS volume (%llu sectors) is larger than target (%llu): "
                "Windows would not mount it. Replug the drive and retry.",
                total, target_sectors);
        return -1;
      }
    }
  }
  return 0;
}

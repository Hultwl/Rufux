#include "mkfs_task.h"
#include "device.h"
#include "exec.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

// First sector of a partition node (sysfs), 0 if unknown. mkfs.ntfs writes
// this into the BPB "hidden sectors" field, which the NTFS boot record uses
// to find bootmgr on BIOS machines; it cannot always work it out itself.
static unsigned long long part_start_sector(const char *dev) {
  const char *base = strrchr(dev, '/');
  base = base ? base + 1 : dev;
  char p[256], buf[64] = {0};
  snprintf(p, sizeof p, "/sys/class/block/%s/start", base);
  FILE *f = fopen(p, "r");
  if (!f) return 0;
  if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
  fclose(f);
  return strtoull(buf, NULL, 10);
}

static unsigned long long sysfs_num(const char *name, const char *file) {
  char p[300], buf[64] = {0};
  snprintf(p, sizeof p, "/sys/class/block/%s/%s", name, file);
  FILE *f = fopen(p, "r");
  if (!f) return 0;
  if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
  fclose(f);
  return strtoull(buf, NULL, 10);
}

// Size in 512-byte sectors of partition `dev`, once the kernel's view has settled.
// Right after sfdisk the node can still show the OLD size, and a filesystem made
// then overflows the new partition: UEFI:NTFS still boots it, but Windows treats
// the volume as corrupt and never mounts it ("a media driver is missing").
// Returns 0 if the size never became consistent with the disk.
static unsigned long long settled_part_sectors(const char *dev) {
  const char *base = strrchr(dev, '/');
  base = base ? base + 1 : dev;
  char link[300], real[PATH_MAX];
  snprintf(link, sizeof link, "/sys/class/block/%s/..", base);
  if (!realpath(link, real)) return 0;
  const char *disk = strrchr(real, '/');
  disk = disk ? disk + 1 : real;
  for (int i = 0; i < 25; i++) {
    unsigned long long sz = sysfs_num(base, "size"), st = sysfs_num(base, "start"), dk = sysfs_num(disk, "size");
    if (sz > 0 && dk > 0 && st + sz <= dk) return sz;
    usleep(200000);
  }
  return 0;
}

int rufux_fs_is_fat16(const char *fs) { return fs && !strcmp(fs, "fat16"); }
int rufux_fs_is_fat(const char *fs) {
  return fs && (!strcmp(fs, "vfat") || !strcmp(fs, "fat32") || !strcmp(fs, "fat16"));
}

// Size in bytes of a block device or image file (0 when unknown).
static unsigned long long dev_bytes(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return 0;
  if (S_ISREG(st.st_mode)) return (unsigned long long)st.st_size;
  unsigned long long b = 0;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    if (ioctl(fd, BLKGETSIZE64, &b) != 0) b = 0;
    close(fd);
  }
  return b;
}

int rufux_format(const char *dst, const RufuxMkfsOpts *o,
                 char *err, unsigned long cap) {
  if (!o->fs || (!strcmp(o->fs, "") )) { snprintf(err, cap, "need --fs"); return -1; }
  if (!o->dry_run && !o->yes) { snprintf(err, cap, "refusing real format without --yes"); return -1; }
  // Fail fast on over-long labels (vfat 11, exfat 15, ext 16, ntfs/udf 32)
  // instead of partitioning first and dying in mkfs.
  if (o->label && o->label[0]) {
    size_t max = 32;
    if (rufux_fs_is_fat(o->fs)) max = 11;
    else if (!strcmp(o->fs, "exfat")) max = 15;
    else if (!strcmp(o->fs, "ext4") || !strcmp(o->fs, "ext2") || !strcmp(o->fs, "ext3")) max = 16;
    if (strlen(o->label) > max) {
      snprintf(err, cap, "label '%s' too long for %s (max %zu chars)", o->label, o->fs, max);
      return -1;
    }
  }
  if (rufux_check_target(dst, o->allow_fixed, o->allow_file, err, cap) != 0) return -1;

  const char **av = NULL;

  // Function-local argv buffers (reentrant; no shared static state).
  char lab_vfat[160], lab_ntfs[160], start_ntfs[32], clus_ntfs[32], count_ntfs[32];
  unsigned long long part_sectors = 0;
  char lab_exfat[160], lab_ext4[160], sec_vfat[32];
  // mkfs.ntfs -F -Q -p S -H 255 -S 63 [-c N] [-L label] dev NULL = 16 slots
  const char *a_vfat[9], *a_ntfs[18], *a_exfat[5], *a_ext4[9], *a_udf[4];
  if (rufux_fs_is_fat(o->fs)) {
    if (!rufux_have("mkfs.vfat")) { snprintf(err, cap, "mkfs.vfat missing"); return -1; }
    int fat16 = rufux_fs_is_fat16(o->fs);
    int i = 0;
    a_vfat[i++] = "mkfs.vfat"; a_vfat[i++] = "-F"; a_vfat[i++] = fat16 ? "16" : "32";
    int spc = o->cluster_sectors;
    if (fat16) {
      // FAT16 tops out at 65524 clusters of at most 64 KiB, so a volume over 4 GiB
      // cannot exist, and one over 2 GiB needs 64 KiB clusters (128 sectors).
      unsigned long long bytes = dev_bytes(dst);
      if (bytes > (4ULL << 30)) {
        snprintf(err, cap, "FAT16 volumes are limited to 4 GiB ('%s' is %.1f GiB); use FAT32",
                 dst, bytes / 1073741824.0);
        return -1;
      }
      if (spc <= 0 && bytes > (2ULL << 30)) spc = 128;
    }
    if (spc > 0) {
      snprintf(sec_vfat, sizeof sec_vfat, "%d", spc);
      a_vfat[i++] = "-s"; a_vfat[i++] = sec_vfat;
    }
    if (o->label && o->label[0]) { snprintf(lab_vfat, sizeof lab_vfat, "%s", o->label); a_vfat[i++] = "-n"; a_vfat[i++] = lab_vfat; }
    a_vfat[i++] = dst; a_vfat[i] = NULL; av = a_vfat;
  } else if (!strcmp(o->fs, "ntfs")) {
    if (!rufux_have("mkfs.ntfs")) { snprintf(err, cap, "mkfs.ntfs missing (ntfsprogs)"); return -1; }
    int i = 0;
    a_ntfs[i++] = "mkfs.ntfs"; a_ntfs[i++] = "-F"; a_ntfs[i++] = "-Q";
    // mkntfs only fills the BPB geometry fields it can work out for itself,
    // and on most USB sticks HDIO_GETGEO gives it nothing: it then prints
    // "Windows will not be able to boot from this device" and writes 0 for
    // hidden sectors / heads / sectors-per-track. Windows is unhappy with a
    // volume described that way, so supply all three explicitly, the way the
    // Windows formatter does (255 heads, 63 sectors per track).
    unsigned long long st = part_start_sector(dst);
    snprintf(start_ntfs, sizeof start_ntfs, "%llu", st ? st : 2048ULL);
    a_ntfs[i++] = "-p"; a_ntfs[i++] = start_ntfs;
    a_ntfs[i++] = "-H"; a_ntfs[i++] = "255";
    a_ntfs[i++] = "-S"; a_ntfs[i++] = "63";
    if (o->cluster_sectors > 0) {
      // mkntfs takes bytes, the UI carries sectors (512 bytes each).
      snprintf(clus_ntfs, sizeof clus_ntfs, "%d", o->cluster_sectors * 512);
      a_ntfs[i++] = "-c"; a_ntfs[i++] = clus_ntfs;  // was silently dropped
    }
    if (o->label && o->label[0]) { snprintf(lab_ntfs, sizeof lab_ntfs, "%s", o->label); a_ntfs[i++] = "-L"; a_ntfs[i++] = lab_ntfs; }
    a_ntfs[i++] = dst;
    if (!o->dry_run) {
      part_sectors = settled_part_sectors(dst);
      if (part_sectors == 0 && strstr(dst, "/dev/")) {
        snprintf(err, cap, "the kernel reports an inconsistent size for '%s' (partition table not re-read); "
                           "unplug and replug the drive and try again", dst);
        return -1;
      }
      // The exact sector count, so the volume can never outgrow its partition.
      if (part_sectors) { snprintf(count_ntfs, sizeof count_ntfs, "%llu", part_sectors); a_ntfs[i++] = count_ntfs; }
    }
    a_ntfs[i] = NULL; av = a_ntfs;
  } else if (!strcmp(o->fs, "exfat")) {
    if (!rufux_have("mkfs.exfat")) { snprintf(err, cap, "mkfs.exfat missing"); return -1; }
    int i = 0;
    a_exfat[i++] = "mkfs.exfat";
    if (o->label && o->label[0]) { snprintf(lab_exfat, sizeof lab_exfat, "%s", o->label); a_exfat[i++] = "-n"; a_exfat[i++] = lab_exfat; }
    a_exfat[i++] = dst; a_exfat[i] = NULL; av = a_exfat;
  } else if (!strcmp(o->fs, "ext4") || !strcmp(o->fs, "ext2") || !strcmp(o->fs, "ext3")) {
    if (!rufux_have("mkfs.ext4")) { snprintf(err, cap, "mkfs.ext4 missing"); return -1; }
    int i = 0;
    // mke2fs picks the file system from -t when given, whatever its own name is,
    // so ext2/ext3 no longer end up as ext4 (they used to).
    a_ext4[i++] = "mkfs.ext4"; a_ext4[i++] = "-t"; a_ext4[i++] = o->fs; a_ext4[i++] = "-F";
    if (o->label && o->label[0]) { snprintf(lab_ext4, sizeof lab_ext4, "%s", o->label); a_ext4[i++] = "-L"; a_ext4[i++] = lab_ext4; }
    a_ext4[i++] = dst; a_ext4[i] = NULL; av = a_ext4;
  } else if (!strcmp(o->fs, "udf")) {
    if (!rufux_have("mkfs.udf")) { snprintf(err, cap, "mkfs.udf missing"); return -1; }
    a_udf[0] = "mkfs.udf"; a_udf[1] = dst; a_udf[2] = NULL; av = a_udf;
  } else {
    if (!strcmp(o->fs, "refs") || !strcmp(o->fs, "refsv1")) {
      snprintf(err, cap, "ReFS has no Linux formatter (proprietary filesystem); use ntfs");
      return -1;
    }
    snprintf(err, cap, "unsupported fs '%s' (vfat|fat16|ntfs|exfat|ext2|ext3|ext4|udf)", o->fs);
    return -1;
  }
  if (rufux_run(av, o->dry_run) != 0) {
    if (!o->dry_run) snprintf(err, cap, "mkfs.%s failed on '%s'", o->fs, dst);
    return o->dry_run ? 0 : -1;
  }
  if (!o->dry_run && !strcmp(o->fs, "ntfs") && part_sectors) {
    unsigned char bs[512];
    int fd = open(dst, O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? pread(fd, bs, sizeof bs, 0) : -1;
    if (fd >= 0) close(fd);
    if (n == (ssize_t)sizeof bs) {
      unsigned long long total = 0;
      for (int k = 7; k >= 0; k--) total = (total << 8) | bs[0x28 + k];
      if (total > part_sectors) {
        snprintf(err, cap, "NTFS volume (%llu sectors) is larger than its partition (%llu): "
                           "Windows would not mount it. Replug the drive and retry.", total, part_sectors);
        return -1;
      }
    }
  }
  return 0;
}

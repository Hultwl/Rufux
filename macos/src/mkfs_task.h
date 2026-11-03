#ifndef RUFUX_MKFS_H
#define RUFUX_MKFS_H
// mkfs dispatch for macOS: native newfs_{msdos,exfat,udf}, vendored mkntfs, and e2fsprogs mke2fs.
typedef struct {
  const char *fs; // vfat|fat32|fat16|ntfs|exfat|ext2|ext3|ext4|udf
  const char *label; // may be NULL
  int cluster_sectors; // 0 = default (512-byte sectors per cluster)
  int dry_run;
  int allow_fixed;
  int allow_file;
  int yes;
} RufuxMkfsOpts;

int rufux_format(const char *dst, const RufuxMkfsOpts *o, char *err, unsigned long cap);

int rufux_format_ext_from_dir(const char *dst, const RufuxMkfsOpts *o,
                              const char *root_dir, char *err, unsigned long cap);

// 1 for any FAT flavour (vfat/fat32 = FAT32, fat16), 0 otherwise.
int rufux_fs_is_fat(const char *fs);

// 1 only for FAT16.
int rufux_fs_is_fat16(const char *fs);
#endif

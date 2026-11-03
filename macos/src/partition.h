#ifndef RUFUX_PARTITION_H
#define RUFUX_PARTITION_H
// Pure-C GPT/MBR layout writer for macOS (replaces Linux sfdisk).
// Layouts: "single" (one partition), "esp+main" (ESP + main) or
// "main+uefintfs" (data partition + 1 MiB UEFI:NTFS partition at the end).
typedef struct {
  const char *scheme;  // "gpt" | "dos" ("mbr" accepted as alias for "dos")
  const char *layout;  // "single" | "esp+main" | "main+uefintfs"
  const char *fs_main; // MBR partition type source (vfat->0c, fat16->0e, ntfs/exfat/udf->07, ext*->83); NULL = 0c
  // GPT only: add a 1 MiB BIOS boot partition at the end for GRUB 2's core.img.
  // Ignored for "dos"/"mbr".
  int bios_boot;
  int dry_run;
  int allow_fixed;
  int allow_file;
  int yes;
} RufuxPartOpts;
int rufux_partition(const char *dst, const RufuxPartOpts *o, char *err, unsigned long cap);
void rufux_partition_plan(const char *dst, const RufuxPartOpts *o, char *out, unsigned long cap);
int rufux_part_offset_sectors(const char *disk, int slice,
                              unsigned long long *start_lba, unsigned long long *count_lba);
#endif

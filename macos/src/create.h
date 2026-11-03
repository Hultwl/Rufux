#ifndef RUFUX_CREATE_H
#define RUFUX_CREATE_H
// High-level flows shared by CLI `create` and the macOS Tauri GUI START button.
// Mirrors Rufus's operation order: bad-blocks check -> partition ->
// format -> extract/populate -> bootloader -> validation.

typedef void (*RufuxCreateProgress)(unsigned long long done,
                                    unsigned long long total, void *user);
typedef void (*RufuxCreateLog)(const char *msg, void *user);

typedef struct {
  const char *mode;       // "dd" | "extract" | "format" | "dos" | "windows"
  const char *scheme;     // "gpt" | "dos" ("mbr" accepted as alias)
  const char *fs;         // vfat|fat32|fat16|ntfs|exfat|ext2|ext3|ext4|udf
  const char *label;      // may be NULL
  const char *wue;        // windows mode: bypass,nro,privacy,bitlocker,locale,qol,user=NAME,all,none
  unsigned split_wim_mb;  // windows mode: split install.wim into parts of this many MiB (0 = auto on FAT32)
  const char *locale, *keyboard, *timezone;
  unsigned long persist_mb;
  int cluster_sectors;    // 0 = default
  int quick_format;       // 0 = zero first 16 MiB before real block flows
  int extended_label;     // autorun.inf on extract flows
  int uefi_validate;      // validate EFI bootloader after extract
  int badblock_passes;    // 0 = skip
  int verify;             // dd mode: re-read and compare
  int ignore_smart;       // write even when the drive reports a failing SMART status
  int dry_run;
  int allow_fixed;
  int allow_file;
  int yes;
} RufuxCreateOpts;

void rufux_create_defaults(RufuxCreateOpts *o);
int rufux_create(const char *src, const char *dst, const RufuxCreateOpts *o,
                 RufuxCreateProgress prog, void *puser,
                 RufuxCreateLog log, void *luser,
                 char *err, unsigned long errcap);

#endif

#ifndef RUFUX_CREATE_H
#define RUFUX_CREATE_H
// High-level flows shared by CLI `create` and the GUI START button.
// Mirrors Rufus's operation order: bad-blocks check -> partition ->
// format -> extract/write -> bootloader -> validation.
typedef void (*RufuxCreateProgress)(unsigned long long done,
                                    unsigned long long total, void *user);
typedef void (*RufuxCreateLog)(const char *msg, void *user);

typedef struct {
  const char *mode; // "dd" | "extract" | "format" | "dos" | "windows"
  const char *scheme; // "gpt" | "dos"
  const char *fs; // vfat|ntfs|exfat|ext4|udf
  const char *label; // may be NULL
  const char *wue; // windows mode: comma list bypass,nro,privacy,all,none (NULL = nothing)
  unsigned split_wim_mb; // windows mode: split install.wim into parts of this many MiB (0 = only when needed for FAT32)
  const char *locale, *keyboard, *timezone; // windows mode: values for the "locale" wue item
  unsigned long persist_mb;
  int cluster_sectors; // 0 = default
  int quick_format; // 0 = zero first 16MB before real block flows
  int extended_label; // autorun.inf on extract flows
  int uefi_validate; // validate EFI bootloader after extract
  int badblock_passes; // 0 = skip
  int verify; // dd mode: re-read and compare
  int ignore_smart; // write even when the drive reports a failing SMART status
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

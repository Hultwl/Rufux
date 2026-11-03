#ifndef RUFUX_BOOT_H
#define RUFUX_BOOT_H
// MBR install preserving partition table + syslinux helper.
typedef struct {
  const char *kind; // "bios" (mbr.bin) | "gpt" (gptmbr.bin)
  int dry_run;
  int allow_fixed;
  int allow_file;
  int yes;
} RufuxBootOpts;
int rufux_install_mbr(const char *dst, const RufuxBootOpts *o, char *err, unsigned long cap);
// Run syslinux --install on an already-mounted vfat partition dir marker:
// writes ldlinux.sys via syslinux binary onto block partition dev.
int rufux_install_syslinux(const char *part_dev, int dry_run, char *err, unsigned long cap);
#endif

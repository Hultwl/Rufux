#ifndef RUFUX_ISO_H
#define RUFUX_ISO_H
// ISO/UDF detail probe and Windows install media detection.

typedef struct {
  char label[64];
  unsigned long long size_bytes;
  int valid_iso;   // PVD magic present
  int bootable;    // El Torito boot record present
  int has_efi;     // EFI boot image hint (eltorito platform 0xEF or BOOT.CAT ref)
  int is_windows;  // sources/install.wim|esd|swm present (1) / 0 no / -1 unknown
  int is_udf;      // UDF filesystem present (needs 7z backend, not bsdtar)
} RufuxIsoInfo;

int rufux_probe_iso(const char *path, char *label_out, unsigned long cap);
int rufux_probe_iso_detail(const char *path, RufuxIsoInfo *info);
void rufux_print_iso_detail(const char *path, const RufuxIsoInfo *info);
// 1 = UDF anchor found, 0 = no, -1 = unreadable.
int rufux_iso_is_udf(const char *path);
// 1 if the ISO looks like Windows install media (sources/install.wim|esd|swm).
int rufux_is_windows_iso(const char *iso, char *err, unsigned long cap);
#endif

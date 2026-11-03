#ifndef RUFUX_WININSTALL_H
#define RUFUX_WININSTALL_H
#include "iso_probe.h"

// Stage the UEFI:NTFS ESP payload (loaders + EFI/Rufus NTFS/exFAT drivers) into tmpdir/esp.
int rufux_stage_uefi_ntfs(const char *tmpdir, char *err, unsigned long cap);

// Write the 1 MiB UEFI:NTFS FAT image raw onto its own partition and verify via F_NOCACHE read-back.
int rufux_write_uefi_ntfs(const char *part_dev, char *err, unsigned long cap);

// Windows User Experience (WUE), Rufus style.
typedef void (*RufuxWueLog)(const char *msg, void *user);
typedef struct {
  const char *wue;      // bypass,nro,privacy,bitlocker,locale,qol,user=NAME,all,none
  const char *locale;   // language tag, e.g. en-US (NULL = from macOS / environment)
  const char *keyboard; // Windows keyboard id, e.g. 0409:00000409 (NULL = language default)
  const char *timezone; // IANA id or Windows name (NULL = this Mac's /etc/localtime)
} RufuxWueArgs;

int rufux_windows_customize(const char *root, const RufuxWueArgs *args,
                            RufuxWueLog log, void *user,
                            char *err, unsigned long cap);

// Validate options up front (account name, known WUE flags) before touching any disk.
int rufux_windows_check(const RufuxWueArgs *args, char *err, unsigned long cap);

// Write autounattend.xml in `dir` (compatibility / unit-test entry point).
int rufux_write_unattend(const char *dir, const char *wue,
                         char *err, unsigned long cap);

#endif

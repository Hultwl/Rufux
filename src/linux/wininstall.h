#ifndef RUFUX_WININSTALL_H
#define RUFUX_WININSTALL_H
// Windows installation media: ISO extract to NTFS + UEFI:NTFS boot files
// on the ESP + optional unattended-answer (WUE) XML. Full Windows-To-Go
// (WIM apply + BCD store) is out of scope: it needs Windows-licensed
// bits with no Linux-native path.
// 1 if the ISO looks like Windows install media (sources/install.wim|esd).
int rufux_is_windows_iso(const char *iso, char *err, unsigned long cap);
// Stage the UEFI:NTFS ESP payload (the full EFI tree: loaders plus the
// EFI/Rufus NTFS/exFAT drivers the loader refuses to boot without) into
// tmpdir/esp. Asserts bootx64.efi and ntfs_x64.efi land.
int rufux_stage_uefi_ntfs(const char *tmpdir, char *err, unsigned long cap);
// Write the UEFI:NTFS image (a complete 1 MiB FAT filesystem with the EFI
// loaders and NTFS/exFAT drivers) raw onto its own partition and verify it
// by reading it back. No mkfs, no mount.
int rufux_write_uefi_ntfs(const char *part_dev, char *err, unsigned long cap);
// Windows User Experience, Rufus style. `wue` is the comma list
// bypass,nro,privacy,all,none (NULL = bypass); `drivers` is an optional folder
// copied to $WinPEDriver$. See wininstall.c for how each option is applied.
typedef void (*RufuxWueLog)(const char *msg, void *user);
int rufux_windows_customize(const char *root, const char *wue, const char *drivers,
                            RufuxWueLog log, void *user, char *err, unsigned long cap);
// Write autounattend.xml: LabConfig HW bypasses + optional NRO bypass +
// privacy screens off. `wue` is a comma list: bypass,nro,privacy (any
// subset; NULL/empty = bypass only... pass "" for bypass-only default).
int rufux_write_unattend(const char *dir, const char *wue,
                         char *err, unsigned long cap);
#endif

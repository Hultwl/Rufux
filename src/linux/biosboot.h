#ifndef RUFUX_BIOSBOOT_H
#define RUFUX_BIOSBOOT_H
// Legacy BIOS boot for Windows installation media on an MBR drive.
//
// Windows boots on BIOS through boot code in the partition that loads bootmgr.
// For FAT32 that is a stock boot record, but for NTFS it is Microsoft's own
// loader in sectors 1-15 of $Boot, which mkfs.ntfs does not write. Instead of
// either, the drive gets GRUB 2 (i386-pc) in the gap between the MBR and the
// first partition, with a two-line embedded script that finds bootmgr on any
// partition and starts it the way NTLDR would. It is independent of the file
// system (FAT32, NTFS, exFAT) and leaves the Windows files untouched.
typedef void (*RufuxBiosLog)(const char *, void *);

// 1 when the GRUB tools and i386-pc modules needed are installed.
int rufux_bios_available(char *why, unsigned long cap);

// disk: whole-disk node (/dev/sdX). mnt: the mounted first partition. Non-fatal
// for callers: on failure err explains and the drive still boots on UEFI.
int rufux_windows_bios_install(const char *disk, const char *mnt, RufuxBiosLog log, void *luser,
                               char *err, unsigned long cap);
#endif

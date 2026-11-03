#ifndef RUFUX_BIOSBOOT_H
#define RUFUX_BIOSBOOT_H
// Pure-C GRUB 2 (i386-pc) legacy BIOS installer for Windows media on macOS.
// Invokes i686-elf-grub-mkimage to build core.img with an embedded rescue script
// that locates and boots /bootmgr (/BOOTMGR), then patches and writes boot.img
// (0..439, preserving disk signature + MBR/protective-MBR partition table) and
// core.img (into MBR gap LBA 1..N or GPT BIOS boot partition) directly in C.

typedef void (*RufuxBiosLog)(const char *, void *);

// 1 when grub-mkimage (i686-elf-grub-mkimage) and i386-pc modules are installed.
int rufux_bios_available(char *why, unsigned long cap);

// disk: whole-disk node (/dev/diskN) or --allow-file image path.
// mnt: unused on macOS (may be NULL).
int rufux_windows_bios_install(const char *disk, const char *mnt,
                               RufuxBiosLog log, void *luser,
                               char *err, unsigned long cap);

#endif

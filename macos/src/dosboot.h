#ifndef RUFUX_DOSBOOT_H
#define RUFUX_DOSBOOT_H
// FreeDOS bootable disks, macOS-native. Boot-record byte blobs come from
// upstream ms-sys (pure data, no Windows coupling); all file I/O uses
// buffered /dev/disk* (or regular files) for sub-sector seeks. KERNEL.SYS
// must be the first directory entry, so it is always copied before anything
// else onto the fresh filesystem.
int rufux_dos_pbr_fd32(const char *dev, unsigned long long base_off,
                       const char *label11, char *err, unsigned long cap);
int rufux_dos_mbr(const char *disk, char *err, unsigned long cap);
// NTFS partition boot record (ms-sys blobs, POSIX I/O).
int rufux_ntfs_pbr(const char *dev, unsigned long long base_off,
                   char *err, unsigned long cap);
// Locate the FreeDOS payload (RUFUX_RES/freedos, app bundle, or res/freedos in-tree).
// Returns static path or NULL.
const char *rufux_freedos_dir(void);
// Copy the FreeDOS set into a mounted FAT dir, KERNEL.SYS first.
int rufux_dos_copy_files(const char *srcdir, const char *destdir,
                         char *err, unsigned long cap);
#endif

#ifndef RUFUX_PLATFORM_MACOS_H
#define RUFUX_PLATFORM_MACOS_H
#include <stddef.h>

// Directory containing the running executable via _NSGetExecutablePath ("" on failure).
const char *rufux_macos_exe_dir(void);

// Resolves _NSGetExecutablePath, prepends <bundle>/Contents/Resources/bin and
// <exedir> to PATH (or appends vendor/Homebrew toolchain dirs in dev builds),
// and sets default RUFUX_RES and RUFUX_GRUB_DIR when bundled assets exist.
void rufux_macos_init_bundle_env(void);

// Returns file size for regular files, or DKIOCGETBLOCKCOUNT * DKIOCGETBLOCKSIZE
// (with IOKit IOMedia Size fallback) for /dev/disk* and /dev/rdisk*.
unsigned long long rufux_macos_dev_bytes(const char *path);

// Returns DKIOCGETBLOCKSIZE (with IOKit Preferred Block Size fallback; default 512).
unsigned rufux_macos_sector_size(const char *path);

// Maps /dev/disk4s1 -> /dev/rdisk4s1 (leaves regular file paths unchanged).
void rufux_macos_raw_node(const char *dev, char *out, size_t cap);

// Maps /dev/rdisk4s1 -> /dev/disk4s1 (leaves regular file paths unchanged).
void rufux_macos_block_node(const char *dev, char *out, size_t cap);

// Maps /dev/disk4 + 1 -> /dev/disk4s1.
void rufux_macos_part_slice(const char *disk, int slice, char *out, size_t cap);

// Returns 1 if path resolves to the whole disk, slice, or APFS physical backing
// store of "/" (statfs("/", &sfs) -> IOKit parent walk across AppleAPFSContainer /
// IOMedia), or disk0 / disk0s* as a defense-in-depth floor.
int rufux_macos_is_boot_disk(const char *path);

// Runs diskutil unmountDisk force <block_node> on block/raw disk devices.
int rufux_macos_unmount_disk(const char *disk, char *err, size_t cap);

// Attaches a raw disk image via hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount
// (returning /dev/diskK) and detaches via hdiutil detach -force.
int rufux_macos_attach_raw_image(const char *img_path, int readonly, char *disk_out, size_t cap);
int rufux_macos_detach_disk(const char *disk_node);

#endif

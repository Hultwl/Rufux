#ifndef RUFUX_EXEC_H
#define RUFUX_EXEC_H
#include <stddef.h>
#include <sys/types.h>

// Maps canonical tool names ("7z"->"7zz"/"7z", "mkfs.ntfs"->"mkntfs",
// "mkfs.ext4"/"mkfs.ext3"/"mkfs.ext2"->"mke2fs",
// "grub-mkimage"->"i686-elf-grub-mkimage"/"grub-mkimage") against PATH.
// Returns full executable path on success, or NULL if not found.
const char *rufux_resolve_tool(const char *name);

// Run external helpers. dry_run=1 prints only.
int rufux_run(const char *const argv[], int dry_run);
int rufux_have(const char *name); // 1 if executable found via rufux_resolve_tool
int rufux_execvp(const char *file, const char *const argv[]);

// Directory of the running executable ("" on failure).
const char *rufux_exe_dir(void);

// Retrying I/O helpers.
int rufux_pwrite_all(int fd, const void *buf, size_t n, off_t off);
ssize_t rufux_read_full(int fd, void *buf, size_t n);

// fork+exec with stdout+stderr captured. No shell, argv only.
int rufux_capture(const char *const av[], char *out, unsigned long cap);
#endif

#ifndef RUFUX_EXEC_H
#define RUFUX_EXEC_H
#include <stddef.h>
#include <sys/types.h>
// Run external helpers (sfdisk, mkfs.*, bsdtar...). dry_run=1 prints only.
// Pass bare tool names ("sfdisk", "mkfs.vfat"): exec resolves via PATH,
// which keeps this working on split-usr distros (Debian/Ubuntu keep
// mkfs.* in /usr/sbin, Arch merges them into /usr/bin).
int rufux_run(const char *const argv[], int dry_run);
int rufux_have(const char *name); // 1 if executable found via PATH
// Directory of the running executable ("" on failure), for exe-relative
// resource lookup inside relocatable bundles (AppImage: <exedir>/../share).
const char *rufux_exe_dir(void);

// Write exactly n bytes at `off`, or read up to n bytes, retrying when a signal
// interrupts the call and continuing after short transfers. rufux_pwrite_all
// returns 0 on success, -1 on error (errno set). rufux_read_full returns the
// number of bytes read (less than n only at end of file) or -1 on error.
int rufux_pwrite_all(int fd, const void *buf, size_t n, off_t off);
ssize_t rufux_read_full(int fd, void *buf, size_t n);
// fork+execvp with stdout+stderr captured. No shell, argv only.
int rufux_capture(const char *const av[], char *out, unsigned long cap);
#endif

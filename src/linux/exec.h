#ifndef RUFUX_EXEC_H
#define RUFUX_EXEC_H
// Run external helpers (sfdisk, mkfs.*, bsdtar...). dry_run=1 prints only.
// Pass bare tool names ("sfdisk", "mkfs.vfat"): exec resolves via PATH,
// which keeps this working on split-usr distros (Debian/Ubuntu keep
// mkfs.* in /usr/sbin, Arch merges them into /usr/bin).
int rufux_run(const char *const argv[], int dry_run);
int rufux_have(const char *name); // 1 if executable found via PATH
// fork+execvp with stdout+stderr captured. No shell, argv only.
int rufux_capture(const char *const av[], char *out, unsigned long cap);
#endif

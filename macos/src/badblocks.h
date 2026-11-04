#ifndef RUFUX_BADBLOCKS_H
#define RUFUX_BADBLOCKS_H
typedef void (*RufuxScanProgress)(unsigned long long done, unsigned long long total, void *user);
// Read-only surface scan (safe).
int rufux_badblocks(const char *path, int allow_file, RufuxScanProgress cb, void *user,
                    unsigned long long *bad_out, char *err, unsigned long cap);
// Destructive write-pattern test (Rufus-style): writes 0xAA/0x55/0xFF/0x00
// starting at patterns[first % 4] for `passes` (1-4) patterns, re-reads
// and compares per 1MB region. Destroys all data on the target.
int rufux_badblocks_write(const char *path, int allow_file, int passes, int first,
                          RufuxScanProgress cb, void *user,
                          unsigned long long *bad_out, char *err, unsigned long cap);
#endif

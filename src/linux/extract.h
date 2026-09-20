#ifndef RUFUX_EXTRACT_H
#define RUFUX_EXTRACT_H
// ISO -> directory. Backend is chosen by filesystem: UDF images go to
// 7z (bsdtar silently under-extracts some UDF layouts), everything else
// prefers bsdtar with a 7z fallback.
typedef void (*RufuxExtractProgress)(unsigned long long done,
                                     unsigned long long total, void *user);
typedef void (*RufuxExtractLog)(const char *msg, void *user);
// Windows media for FAT32: extract everything, splitting install.wim into
// `split_mb` MiB parts when it is over 4 GiB (or always when `force`).
int rufux_extract_windows_split(const char *src, const char *dest_dir, unsigned split_mb, int force,
                                RufuxExtractProgress prog, void *user, RufuxExtractLog log,
                                void *log_user, char *err, unsigned long cap);
int rufux_extract_iso(const char *src, const char *dest_dir, int dry_run,
                      char *err, unsigned long cap);
// Same, but reports destination growth against the image size.
int rufux_extract_iso_progress(const char *src, const char *dest_dir, int dry_run,
                               RufuxExtractProgress prog, void *user,
                               char *err, unsigned long cap);
// Extended label: write autorun.inf carrying the volume label (Rufus parity).
int rufux_write_autorun(const char *dir, const char *label, int dry_run,
                        char *err, unsigned long cap);
#endif

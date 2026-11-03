#ifndef RUFUX_EXTRACT_H
#define RUFUX_EXTRACT_H
// ISO -> directory extraction and macOS read-only ISO mounting / WIM splitting.
// UDF images go directly to 7z (7zz); ISO9660/Joliet prefers bsdtar with 7z fallback.

typedef void (*RufuxExtractProgress)(unsigned long long done,
                                     unsigned long long total, void *user);
typedef void (*RufuxExtractLog)(const char *msg, void *user);

// Windows media for FAT32: extract everything, splitting install.wim into
// `split_mb` MiB parts when it is over 4 GiB (or always when `force` != 0).
int rufux_extract_windows_split(const char *src, const char *dest_dir,
                                unsigned split_mb, int force,
                                RufuxExtractProgress prog, void *user,
                                RufuxExtractLog log, void *log_user,
                                char *err, unsigned long cap);

int rufux_extract_iso(const char *src, const char *dest_dir, int dry_run,
                      char *err, unsigned long cap);

int rufux_extract_iso_progress(const char *src, const char *dest_dir, int dry_run,
                               RufuxExtractProgress prog, void *user,
                               char *err, unsigned long cap);

// Extended label: write autorun.inf carrying the volume label (Rufus parity).
int rufux_write_autorun(const char *dir, const char *label, int dry_run,
                        char *err, unsigned long cap);

// Helpers shared with hybrid userspace NTFS Windows creation in create.c:
int rufux_archive_find_entry(const char *iso, const char *relpath_ci,
                             char *actual_path_out, unsigned long path_cap,
                             unsigned long long *size_out);

int rufux_extract_iso_excluding(const char *src, const char *dest_dir,
                                const char *exclude_entry,
                                unsigned long long expect_bytes,
                                RufuxExtractProgress prog, void *user,
                                char *err, unsigned long cap);

int rufux_extract_single_entry(const char *src, const char *dest_dir,
                               const char *entry_path,
                               unsigned long long expect_bytes,
                               RufuxExtractProgress prog, void *user,
                               char *err, unsigned long cap);

int rufux_iso_mount_ro(const char *iso, char *mnt_out, unsigned long mnt_cap,
                       char *dev_out, unsigned long dev_cap);
void rufux_iso_umount_ro(const char *mnt, const char *dev);

// Scratch directory helper checking available disk space (RUFUX_TMPDIR, TMPDIR,
// directory of `src`, /var/tmp, /tmp).
int rufux_scratch_dir(unsigned long long need, const char *src,
                      char *out, unsigned long cap);

#endif

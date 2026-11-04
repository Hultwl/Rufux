#ifndef RUFUX_NTFS_POPULATE_H
#define RUFUX_NTFS_POPULATE_H
// Direct userspace libntfs-3g directory & large-file populator for macOS.

typedef void (*RufuxNtfsProgress)(unsigned long long done,
                                  unsigned long long total,
                                  void *user);

int rufux_ntfs_populate_dir(const char *ntfs_target, const char *src_dir,
                            RufuxNtfsProgress prog, void *user,
                            char *err, unsigned long cap);

int rufux_ntfs_write_single_file(const char *ntfs_target, const char *src_file,
                                 const char *ntfs_relpath,
                                 RufuxNtfsProgress prog, void *user,
                                 char *err, unsigned long cap);

#endif

#ifndef RUFUX_MOUNT_H
#define RUFUX_MOUNT_H

int rufux_mount(const char *dev, int dry_run, char *mnt_out, unsigned long cap,
                char *err, unsigned long errcap);
int rufux_unmount(const char *dev, int dry_run, char *err, unsigned long errcap);
void rufux_part1(const char *disk, char *out, unsigned long cap);
int rufux_mount_is_whole_disk_active(const char *whole_sysname);
int rufux_unmount_all_for_disk(const char *disk, char *err, unsigned long errcap);

#endif

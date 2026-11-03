#ifndef RUFUX_VDISK_H
#define RUFUX_VDISK_H
// Virtual disk images that are not a raw byte-for-byte copy of a disk: VHDX,
// dynamic VHD, VMDK, QCOW2 and VDI. Rufux does not decode these itself; it
// hands them to `qemu-img convert -O raw`, straight onto the target, so no
// disk-sized temporary file is needed. Fixed VHD (a raw payload plus a
// footer) stays with vhd.c.
#include "writer.h"
typedef struct {
  char format[16];              // qemu-img format name: vhdx | vpc | vmdk | qcow2 | vdi
  char label[24];               // what to call it in messages: VHDX, dynamic VHD, ...
  unsigned long long virtual_size;
} RufuxVdisk;

// 1 = a virtual disk that needs conversion (vd filled in)
// 0 = anything else (raw image, ISO, fixed VHD): use the normal writer
// -1 = recognised but cannot or must not be written (err explains)
int rufux_vdisk_detect(const char *src, RufuxVdisk *vd, char *err, unsigned long cap);

// Converts `src` onto `dst`. progress is reported as done/total in 0.01% steps.
// verify=1 compares the converted result with the source afterwards.
int rufux_vdisk_write(const char *src, const RufuxVdisk *vd, const char *dst,
                      const RufuxWriteOpts *opts, RufuxWriteProgress cb, void *user,
                      char *err, unsigned long cap);
#endif

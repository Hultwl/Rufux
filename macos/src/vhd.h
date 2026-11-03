#ifndef RUFUX_VHD_H
#define RUFUX_VHD_H
// Fixed VHD support: a fixed virtual disk is a raw payload with a
// 512-byte footer ("conectix"). Dynamic/differencing and VHDX are
// refused with a message (convert with qemu-img first).
#if __has_include("writer.h")
#include "writer.h"
#elif !defined(RUFUX_WRITER_H)
#define RUFUX_WRITER_H
typedef void (*RufuxWriteProgress)(unsigned long long done,
                                   unsigned long long total, void *user);
typedef struct {
  int dry_run;
  int verify;
  int allow_fixed;
  int allow_file;
  int yes;
  RufuxWriteProgress vprog;
  void *vuser;
  unsigned long long src_len;
} RufuxWriteOpts;
#endif

typedef struct {
  int is_vhd; // footer cookie present
  int is_fixed; // fixed type (only directly writable kind)
  unsigned long long payload_bytes; // file size minus footer
  char kind[32]; // "fixed", "dynamic", "differencing", "vhdx", "corrupt", "unknown"
} RufuxVhdInfo;

int rufux_vhd_probe(const char *path, RufuxVhdInfo *info);
// If src is a fixed VHD, cap the write payload (skip footer) and log it.
// Refuses dynamic/differencing/VHDX/corrupt. No-ops for non-VHD files.
int rufux_vhd_adjust(const char *src, RufuxWriteOpts *o,
                     void (*log)(const char *, void *), void *luser,
                     char *err, unsigned long cap);
#endif

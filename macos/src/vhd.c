#include "vhd.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int rufux_vhd_probe(const char *path, RufuxVhdInfo *info) {
  memset(info, 0, sizeof *info);
  snprintf(info->kind, sizeof info->kind, "unknown");
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  if (st.st_size < 512 + 512) return 0; // too small for fixed VHD
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  unsigned char head[8] = {0};
  if (fread(head, 1, sizeof head, f) == sizeof head && !memcmp(head, "vhdxfile", 8)) {
    fclose(f);
    snprintf(info->kind, sizeof info->kind, "vhdx");
    return 0;
  }
  unsigned char foot[512];
  if (fseek(f, -512L, SEEK_END) != 0) { fclose(f); return -1; }
  if (fread(foot, 1, sizeof foot, f) != sizeof foot) { fclose(f); return -1; }
  fclose(f);
  if (!memcmp(foot, "vhdxfile", 8)) {
    snprintf(info->kind, sizeof info->kind, "vhdx");
    return 0;
  }
  if (memcmp(foot, "conectix", 8)) return 0; // not a VHD at all
  info->is_vhd = 1;
  // Verify footer checksum: one's complement of the byte sum (checksum
  // field itself at offset 64 treated as zero).
  unsigned sum = 0;
  for (int i = 0; i < 512; i++) sum += (i >= 64 && i < 68) ? 0 : foot[i];
  if ((~sum & 0xFFFFFFFFu) != ((unsigned)foot[64] << 24 | (unsigned)foot[65] << 16 |
                               (unsigned)foot[66] << 8 | foot[67])) {
    snprintf(info->kind, sizeof info->kind, "corrupt");
    return 0;
  }
  unsigned type = (unsigned)foot[60] << 24 | (unsigned)foot[61] << 16 |
                  (unsigned)foot[62] << 8 | foot[63];
  unsigned long long cur = 0;
  for (int i = 0; i < 8; i++) cur = (cur << 8) | foot[48 + i];
  if (type == 2) {
    info->is_fixed = 1;
    info->payload_bytes = (unsigned long long)st.st_size - 512;
    (void)cur;
    snprintf(info->kind, sizeof info->kind, "fixed");
  } else if (type == 3) {
    snprintf(info->kind, sizeof info->kind, "dynamic");
  } else if (type == 4) {
    snprintf(info->kind, sizeof info->kind, "differencing");
  }
  return 0;
}

int rufux_vhd_adjust(const char *src, RufuxWriteOpts *o,
                     void (*log)(const char *, void *), void *luser,
                     char *err, unsigned long cap) {
  RufuxVhdInfo info;
  if (rufux_vhd_probe(src, &info) != 0) return 0;
  if (!info.is_vhd && strcmp(info.kind, "vhdx") && strcmp(info.kind, "corrupt")) return 0;
  if (!strcmp(info.kind, "vhdx")) {
    snprintf(err, cap, "VHDX is not directly writable; convert first: qemu-img convert -O raw '%s' out.img", src);
    return -1;
  }
  if (!strcmp(info.kind, "corrupt")) {
    snprintf(err, cap, "'%s' has a VHD cookie but a bad footer checksum", src);
    return -1;
  }
  if (!info.is_fixed) {
    snprintf(err, cap, "%s VHD is not directly writable (only fixed type is); convert first: qemu-img convert -O raw '%s' out.img",
             info.kind, src);
    return -1;
  }
  o->src_len = info.payload_bytes;
  if (log) {
    char m[256];
    snprintf(m, sizeof m, "Fixed VHD detected: writing %llu payload bytes (512B footer skipped).",
             info.payload_bytes);
    log(m, luser);
  }
  return 0;
}

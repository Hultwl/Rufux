#define _GNU_SOURCE
#include "dosboot.h"
#include "exec.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>

// Upstream ms-sys boot-record blobs (data only).
#include "../ms-sys/inc/br_fat32_0x0.h"
#include "../ms-sys/inc/br_fat32fd_0x52.h"
#include "../ms-sys/inc/br_fat32fd_0x3f0.h"
#include "../ms-sys/inc/br_ntfs_0x0.h"
#include "../ms-sys/inc/br_ntfs_0x54.h"
#include "../ms-sys/inc/mbr_dos.h"

static int blob_write(FILE *f, unsigned long long off,
                      const unsigned char *blob, size_t len,
                      const char *what, char *err, unsigned long cap) {
  if (fseeko(f, (off_t)off, SEEK_SET) != 0) {
    snprintf(err, cap, "seek failed writing %s", what);
    return -1;
  }
  if (fwrite(blob, 1, len, f) != len) {
    snprintf(err, cap, "write failed (%s)", what);
    return -1;
  }
  return 0;
}

int rufux_dos_pbr_fd32(const char *dev, unsigned long long base_off,
                       const char *label11, char *err, unsigned long cap) {
  FILE *f = fopen(dev, "r+b");
  if (!f) { snprintf(err, cap, "cannot open '%s': %s", dev, strerror(errno)); return -1; }
  int rc = -1;
  unsigned char label[11];
  memset(label, ' ', sizeof label);
  for (int i = 0; label11 && label11[i] && i < 11; i++)
    label[i] = toupper((unsigned char)label11[i]);
  // Mirror ms-sys write_fat_32_fd_br (!bKeepLabel): jump/OEM, label,
  // boot code, second-stage loader. BPB bytes inside the blobs' gaps
  // are left to the formatter (same layout ms-sys assumes).
  if (blob_write(f, base_off + 0x0, br_fat32_0x0, sizeof br_fat32_0x0, "pbr head", err, cap) == 0 &&
      blob_write(f, base_off + 0x47, label, sizeof label, "pbr label", err, cap) == 0 &&
      blob_write(f, base_off + 0x52, br_fat32_0x52, sizeof br_fat32_0x52, "pbr code", err, cap) == 0 &&
      blob_write(f, base_off + 0x3f0, br_fat32_0x3f0, sizeof br_fat32_0x3f0, "pbr tail", err, cap) == 0)
    rc = 0;
  if (fflush(f) != 0) rc = -1;
  fclose(f);
  return rc;
}

int rufux_dos_mbr(const char *disk, char *err, unsigned long cap) {  FILE *f = fopen(disk, "r+b");
  if (!f) { snprintf(err, cap, "cannot open '%s': %s", disk, strerror(errno)); return -1; }
  // First 440 bytes are boot code; partition table (446+) untouched.
  size_t n = sizeof mbr_dos_0x0 < 440 ? sizeof mbr_dos_0x0 : 440;
  int rc = blob_write(f, 0, mbr_dos_0x0, n, "dos mbr", err, cap);
  if (fflush(f) != 0) rc = -1;
  fclose(f);
  return rc;
}

int rufux_ntfs_pbr(const char *dev, unsigned long long base_off,
                   char *err, unsigned long cap) {
  FILE *f = fopen(dev, "r+b");
  if (!f) { snprintf(err, cap, "cannot open '%s': %s", dev, strerror(errno)); return -1; }
  // Mirror ms-sys write_ntfs_br: jump/OEM head + boot code, BPB preserved.
  int rc = -1;
  if (blob_write(f, base_off + 0x0, br_ntfs_0x0, sizeof br_ntfs_0x0, "ntfs pbr head", err, cap) == 0 &&
      blob_write(f, base_off + 0x54, br_ntfs_0x54, sizeof br_ntfs_0x54, "ntfs pbr code", err, cap) == 0)
    rc = 0;
  if (fflush(f) != 0) rc = -1;
  fclose(f);
  return rc;
}

const char *rufux_freedos_dir(void) {
  static char path[1024];
  // Test/deploy override first.
  const char *env = getenv("RUFUX_RES");
  if (env && env[0]) {
    snprintf(path, sizeof path, "%s/freedos", env);
    DIR *d = opendir(path);
    if (d) { closedir(d); return path; }
  }
  static const char *cands[] = {
    "res/freedos", // build tree
    "/usr/share/rufux/freedos",
    "/usr/local/share/rufux/freedos",
    NULL,
  };
  // Relative to the executable (AppImage / installed bin).
  const char *exedir = rufux_exe_dir();
  char probe[1152];
  if (exedir[0]) {
    snprintf(probe, sizeof probe, "%s/../share/rufux/freedos", exedir);
    DIR *d = opendir(probe);
    if (d) { closedir(d); snprintf(path, sizeof path, "%s", probe); return path; }
  }
  for (int i = 0; cands[i]; i++) {
    DIR *d = opendir(cands[i]);
    if (d) { closedir(d); snprintf(path, sizeof path, "%s", cands[i]); return path; }
  }
  return NULL;
}

int rufux_dos_copy_files(const char *srcdir, const char *destdir,
                         char *err, unsigned long cap) {
  // KERNEL.SYS first: DOS boots it by directory order on fresh media.
  const char *first[] = {"KERNEL.SYS", "COMMAND.COM", NULL};
  DIR *d = opendir(srcdir);
  if (!d) { snprintf(err, cap, "freedos dir '%s' missing", srcdir); return -1; }
  for (int i = 0; first[i]; i++) {
    char s[1152], t[1152];
    snprintf(s, sizeof s, "%s/%s", srcdir, first[i]);
    snprintf(t, sizeof t, "%s/%s", destdir, first[i]);
    if (access(s, R_OK) != 0) {
      snprintf(err, cap, "freedos payload lacks %s", first[i]);
      closedir(d);
      return -1;
    }
    const char *av[] = {"cp", "--", s, t, NULL};
    if (rufux_run(av, 0) != 0) {
      snprintf(err, cap, "copy failed for %s", first[i]);
      closedir(d);
      return -1;
    }
  }
  // Then everything else (skip readme + already-copied).
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    if (!strcmp(e->d_name, "KERNEL.SYS") || !strcmp(e->d_name, "COMMAND.COM") ||
        !strcmp(e->d_name, "readme.txt"))
      continue;
    char s[1152], t[1152];
    snprintf(s, sizeof s, "%s/%s", srcdir, e->d_name);
    snprintf(t, sizeof t, "%s/%s", destdir, e->d_name);
    const char *av[] = {"cp", "--", s, t, NULL};
    if (rufux_run(av, 0) != 0) {
      snprintf(err, cap, "copy failed for %s", e->d_name);
      closedir(d);
      return -1;
    }
  }
  closedir(d);
  return 0;
}

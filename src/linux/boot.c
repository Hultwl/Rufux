#define _GNU_SOURCE
#include "boot.h"
#include "device.h"
#include "exec.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <ftw.h>
#include <sys/stat.h>

// Recursive fallback: distributions disagree on syslinux file layout,
// so search the known roots for the exact basename instead of guessing.
static char found_path[512];
static const char *want_base;
static int want_cb(const char *path, const struct stat *sb, int type, struct FTW *ftw) {
  (void)sb; (void)ftw;
  if (type == FTW_F) {
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    if (!strcmp(b, want_base) && access(path, R_OK) == 0) {
      snprintf(found_path, sizeof found_path, "%s", path);
      return 1; // stop the walk
    }
  }
  return 0;
}

static const char *find_mbr_file(const char *base) {
  static char relroot[1152];
  const char *roots[8];
  int nroots = 0;
  // Bundled copy first (AppImage: <exedir>/../share/syslinux).
  const char *ed = rufux_exe_dir();
  if (ed[0]) {
    snprintf(relroot, sizeof relroot, "%s/../share/syslinux", ed);
    struct stat st;
    if (stat(relroot, &st) == 0 && S_ISDIR(st.st_mode))
      roots[nroots++] = relroot;
  }
  static const char *sysroots[] = {
    "/usr/lib/syslinux", "/usr/share/syslinux",
    "/usr/local/share/syslinux", "/usr/local/lib/syslinux", NULL};
  for (int i = 0; sysroots[i] && nroots < 7; i++)
    roots[nroots++] = sysroots[i];
  roots[nroots] = NULL;
  want_base = base;
  found_path[0] = 0;
  for (int i = 0; roots[i]; i++) {
    struct stat st;
    if (stat(roots[i], &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    nftw(roots[i], want_cb, 16, FTW_PHYS);
    if (found_path[0]) break;
  }
  return found_path[0] ? found_path : NULL;
}

static const char *mbr_path(const char *kind) {
  // Bundled copy first (AppImage: <exedir>/../share/syslinux).
  static char rel[1152];
  const char *base = (kind && !strcmp(kind, "gpt")) ? "gptmbr.bin" : "mbr.bin";
  const char *ed = rufux_exe_dir();
  if (ed[0]) {
    snprintf(rel, sizeof rel, "%s/../share/syslinux/%s", ed, base);
    if (!access(rel, R_OK)) return rel;
  }
  // Fast path: known layouts (Arch nests under bios/, Debian flattens).
  static const char *gpt_c[] = {
    "/usr/lib/syslinux/bios/gptmbr.bin",
    "/usr/lib/syslinux/gptmbr.bin",
    "/usr/share/syslinux/gptmbr.bin", NULL};
  static const char *bios_c[] = {
    "/usr/lib/syslinux/bios/mbr.bin",
    "/usr/lib/syslinux/mbr.bin",
    "/usr/share/syslinux/bios/mbr.bin",
    "/usr/share/syslinux/mbr.bin", NULL};
  const char **c = (kind && !strcmp(kind, "gpt")) ? gpt_c : bios_c;
  for (int i = 0; c[i]; i++)
    if (!access(c[i], R_OK)) return c[i];
  // Slow path: walk the syslinux roots for the basename.
  return find_mbr_file((kind && !strcmp(kind, "gpt")) ? "gptmbr.bin" : "mbr.bin");
}

int rufux_install_mbr(const char *dst, const RufuxBootOpts *o,
                      char *err, unsigned long cap) {
  if (!o->dry_run && !o->yes) { snprintf(err, cap, "refusing real MBR write without --yes"); return -1; }
  if (rufux_check_target(dst, o->allow_fixed, o->allow_file, err, cap) != 0) return -1;
  const char *mbr = mbr_path(o->kind);
  if (!mbr) {
    snprintf(err, cap, "syslinux MBR binary not found (looked in /usr/lib|share/syslinux; install syslinux)");
    return -1;
  }
  if (o->dry_run) {
    fprintf(stderr, "+ dd MBR %s -> %s (first 440 bytes, table preserved)\n", mbr, dst);
    return 0;
  }
  int fm = open(mbr, O_RDONLY | O_CLOEXEC);
  if (fm < 0) { snprintf(err, cap, "cannot open %s", mbr); return -1; }
  unsigned char code[440];
  ssize_t n = read(fm, code, sizeof code);
  close(fm);
  if (n != (ssize_t)sizeof code) { snprintf(err, cap, "bad MBR binary size"); return -1; }
  int fd = open(dst, O_WRONLY | O_CLOEXEC);
  if (fd < 0) { snprintf(err, cap, "cannot open '%s': %s", dst, strerror(errno)); return -1; }
  ssize_t w = write(fd, code, sizeof code);
  fsync(fd);
  close(fd);
  if (w != (ssize_t)sizeof code) { snprintf(err, cap, "MBR write failed"); return -1; }
  return 0;
}

int rufux_install_syslinux(const char *part_dev, int dry_run,
                           char *err, unsigned long cap) {
  if (!rufux_have("syslinux")) { snprintf(err, cap, "syslinux missing"); return -1; }
  const char *av[] = {"syslinux", "--install", part_dev, NULL};
  if (rufux_run(av, dry_run) != 0) {
    if (!dry_run) snprintf(err, cap, "syslinux --install failed on '%s'", part_dev);
    return dry_run ? 0 : -1;
  }
  return 0;
}

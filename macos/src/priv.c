#include "priv.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int is_macos_disk_chr(const char *path) {
  if (!path) return 0;
  if (!strncmp(path, "/dev/rdisk", 10) || !strncmp(path, "/dev/disk", 9))
    return 1;
  char resolved[PATH_MAX];
  if (realpath(path, resolved)) {
    if (!strncmp(resolved, "/dev/rdisk", 10) || !strncmp(resolved, "/dev/disk", 9))
      return 1;
  }
  return 0;
}

int rufux_need_root_for_block(const char *path, char *err, unsigned long cap) {
  struct stat st;
  if (!path || stat(path, &st) != 0) return 0; // let caller report missing target
  int is_priv_dev = S_ISBLK(st.st_mode) ||
                    (S_ISCHR(st.st_mode) && is_macos_disk_chr(path));
  if (!is_priv_dev) return 0;
  if (geteuid() == 0) return 0;
  snprintf(err, cap, "target '%s' is a disk device: re-run as root "
                     "(sudo rufux ...)", path);
  return -1;
}

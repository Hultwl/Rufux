#include "persist.h"
#include "exec.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

__attribute__((format(printf, 3, 4)))
static void set_err(char *err, unsigned long cap, const char *fmt, ...) {
  if (!err || cap == 0) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, cap, fmt, ap);
  va_end(ap);
}

int rufux_create_persist(const char *dir, const char *label, unsigned long size_mb,
                         int dry_run, char *err, unsigned long cap) {
  if (err && cap > 0) err[0] = 0;
  if (!label || !label[0]) label = "casper-rw";

  if (strchr(label, '/') != NULL || strcmp(label, ".") == 0 || strcmp(label, "..") == 0) {
    set_err(err, cap, "invalid persistence label '%s'", label);
    return -1;
  }
  if (strlen(label) > 16) {
    set_err(err, cap, "persistence label '%s' too long for ext4 (max 16 chars)", label);
    return -1;
  }
  if (size_mb == 0 || size_mb > 16384) {
    set_err(err, cap, "size must be 1..16384 MB");
    return -1;
  }
  struct stat st;
  if (!dir || !dir[0] || stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
    set_err(err, cap, "dir '%s' missing", dir ? dir : "");
    return -1;
  }

  char path[PATH_MAX];
  int pn = snprintf(path, sizeof path, "%s/%s", dir, label);
  if (pn < 0 || (size_t)pn >= sizeof path) {
    set_err(err, cap, "persistence path too long");
    return -1;
  }

  if (dry_run) {
    fprintf(stderr, "+ ftruncate %s %luM && mkfs.ext4 -t ext4 -F -L %s %s\n",
            path, size_mb, label, path);
    return 0;
  }

  // Check prerequisites before creating the persistence file (no stray partial files).
  if (!rufux_have("mkfs.ext4")) {
    set_err(err, cap, "mkfs.ext4 missing");
    return -1;
  }

  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    set_err(err, cap, "open '%s' failed: %s", path, strerror(errno));
    return -1;
  }
  off_t bytes = (off_t)((unsigned long long)size_mb << 20);
  if (ftruncate(fd, bytes) != 0) {
    int e = errno;
    close(fd);
    unlink(path);
    set_err(err, cap, "ftruncate '%s' to %lu MB failed: %s", path, size_mb, strerror(e));
    return -1;
  }
  if (close(fd) != 0) {
    int e = errno;
    unlink(path);
    set_err(err, cap, "close '%s' failed: %s", path, strerror(e));
    return -1;
  }

  char lab[64];
  snprintf(lab, sizeof lab, "%s", label);
  const char *av[] = {"mkfs.ext4", "-t", "ext4", "-F", "-L", lab, path, NULL};
  if (rufux_run(av, 0) != 0) {
    unlink(path);
    set_err(err, cap, "mkfs.ext4 persist failed");
    return -1;
  }
  return 0;
}

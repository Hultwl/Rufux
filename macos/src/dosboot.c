#include "dosboot.h"
#include "exec.h"
#include "platform_macos.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

// Upstream ms-sys boot-record blobs (pure byte arrays).
#include "../../src/ms-sys/inc/br_fat32_0x0.h"
#include "../../src/ms-sys/inc/br_fat32fd_0x52.h"
#include "../../src/ms-sys/inc/br_fat32fd_0x3f0.h"
#include "../../src/ms-sys/inc/br_ntfs_0x0.h"
#include "../../src/ms-sys/inc/br_ntfs_0x54.h"
#include "../../src/ms-sys/inc/mbr_dos.h"

static int pread_exact(int fd, void *buf, size_t len, unsigned long long off) {
  unsigned char *p = (unsigned char *)buf;
  size_t got = 0;
  while (got < len) {
    ssize_t r = pread(fd, p + got, len - got, (off_t)(off + got));
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return -1;
    got += (size_t)r;
  }
  return 0;
}

static int open_buffered_target(const char *target, int *is_dev_out, unsigned *sec_sz_out,
                                char *err, unsigned long cap) {
  if (err && cap > 0) err[0] = 0;
  if (!target || !target[0]) {
    if (err && cap > 0) snprintf(err, cap, "empty target path");
    return -1;
  }
  if (rufux_macos_is_boot_disk(target)) {
    if (err && cap > 0) snprintf(err, cap, "refusing boot/system disk '%s'", target);
    return -1;
  }

  // Always map /dev/rdisk* -> /dev/disk* so sub-sector patching uses buffered device nodes.
  char bnode[PATH_MAX];
  rufux_macos_block_node(target, bnode, sizeof bnode);

  struct stat st;
  if (stat(bnode, &st) != 0) {
    if (err && cap > 0) snprintf(err, cap, "cannot stat '%s': %s", bnode, strerror(errno));
    return -1;
  }
  int is_dev = S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode);
  unsigned sec_sz = is_dev ? rufux_macos_sector_size(bnode) : 512u;
  if (sec_sz < 512u || sec_sz > 4096u) sec_sz = 512u;

  int fd = open(bnode, O_RDWR | O_CLOEXEC);
  if (fd < 0 && errno == EBUSY && is_dev) {
    if (rufux_macos_unmount_disk(bnode, err, cap) != 0) return -1;
    fd = open(bnode, O_RDWR | O_CLOEXEC);
  }
  if (fd < 0) {
    if (err && cap > 0) snprintf(err, cap, "cannot open '%s': %s", bnode, strerror(errno));
    return -1;
  }

  *is_dev_out = is_dev;
  *sec_sz_out = sec_sz;
  return fd;
}

// Writes `blob[0..len)` at byte offset `off`, preserving all surrounding bytes
// (BPB fields, MBR disk signature & partition entries) via sector-aligned RMW on devices.
static int blob_write(int fd, int is_dev, unsigned sec_sz, unsigned long long off,
                      const unsigned char *blob, size_t len,
                      const char *what, char *err, unsigned long cap) {
  if (len == 0) return 0;
  if (!is_dev) {
    if (rufux_pwrite_all(fd, blob, len, (off_t)off) != 0) {
      if (err && cap > 0) snprintf(err, cap, "write failed (%s): %s", what, strerror(errno));
      return -1;
    }
    return 0;
  }

  unsigned char sec[4096];
  size_t done = 0;
  while (done < len) {
    unsigned long long cur = off + (unsigned long long)done;
    unsigned long long sec_base = (cur / sec_sz) * sec_sz;
    size_t in_sec = (size_t)(cur - sec_base);
    size_t take = (size_t)sec_sz - in_sec;
    if (take > len - done) take = len - done;

    if (pread_exact(fd, sec, sec_sz, sec_base) != 0) {
      if (err && cap > 0) snprintf(err, cap, "read failed (%s): %s", what, strerror(errno));
      return -1;
    }
    memcpy(sec + in_sec, blob + done, take);
    if (rufux_pwrite_all(fd, sec, sec_sz, (off_t)sec_base) != 0) {
      if (err && cap > 0) snprintf(err, cap, "write failed (%s): %s", what, strerror(errno));
      return -1;
    }
    done += take;
  }
  return 0;
}

int rufux_dos_pbr_fd32(const char *dev, unsigned long long base_off,
                       const char *label11, char *err, unsigned long cap) {
  int is_dev = 0;
  unsigned sec_sz = 512u;
  int fd = open_buffered_target(dev, &is_dev, &sec_sz, err, cap);
  if (fd < 0) return -1;

  unsigned char label[11];
  memset(label, ' ', sizeof label);
  for (int i = 0; label11 && label11[i] && i < 11; i++)
    label[i] = (unsigned char)toupper((unsigned char)label11[i]);

  int rc = -1;
  if (blob_write(fd, is_dev, sec_sz, base_off + 0x0ULL, br_fat32_0x0, sizeof br_fat32_0x0,
                 "pbr head", err, cap) == 0 &&
      blob_write(fd, is_dev, sec_sz, base_off + 0x47ULL, label, sizeof label,
                 "pbr label", err, cap) == 0 &&
      blob_write(fd, is_dev, sec_sz, base_off + 0x52ULL, br_fat32_0x52, sizeof br_fat32_0x52,
                 "pbr code", err, cap) == 0 &&
      blob_write(fd, is_dev, sec_sz, base_off + 0x3f0ULL, br_fat32_0x3f0, sizeof br_fat32_0x3f0,
                 "pbr tail", err, cap) == 0) {
    rc = 0;
  }
  if (rc == 0 && fsync(fd) != 0) {
    if (err && cap > 0) snprintf(err, cap, "fsync failed writing FreeDOS PBR: %s", strerror(errno));
    rc = -1;
  }
  close(fd);
  return rc;
}

int rufux_dos_mbr(const char *disk, char *err, unsigned long cap) {
  int is_dev = 0;
  unsigned sec_sz = 512u;
  int fd = open_buffered_target(disk, &is_dev, &sec_sz, err, cap);
  if (fd < 0) return -1;

  size_t n = sizeof mbr_dos_0x0 < 440 ? sizeof mbr_dos_0x0 : 440;
  int rc = blob_write(fd, is_dev, sec_sz, 0ULL, mbr_dos_0x0, n, "dos mbr", err, cap);
  if (rc == 0 && fsync(fd) != 0) {
    if (err && cap > 0) snprintf(err, cap, "fsync failed writing DOS MBR: %s", strerror(errno));
    rc = -1;
  }
  close(fd);
  return rc;
}

int rufux_ntfs_pbr(const char *dev, unsigned long long base_off,
                   char *err, unsigned long cap) {
  int is_dev = 0;
  unsigned sec_sz = 512u;
  int fd = open_buffered_target(dev, &is_dev, &sec_sz, err, cap);
  if (fd < 0) return -1;

  int rc = -1;
  if (blob_write(fd, is_dev, sec_sz, base_off + 0x0ULL, br_ntfs_0x0, sizeof br_ntfs_0x0,
                 "ntfs pbr head", err, cap) == 0 &&
      blob_write(fd, is_dev, sec_sz, base_off + 0x54ULL, br_ntfs_0x54, sizeof br_ntfs_0x54,
                 "ntfs pbr code", err, cap) == 0) {
    rc = 0;
  }
  if (rc == 0 && fsync(fd) != 0) {
    if (err && cap > 0) snprintf(err, cap, "fsync failed writing NTFS PBR: %s", strerror(errno));
    rc = -1;
  }
  close(fd);
  return rc;
}

static int resolve_existing_dir(const char *cand, char *out, size_t cap) {
  if (!cand || !cand[0]) return 0;
  DIR *d = opendir(cand);
  if (!d) return 0;
  closedir(d);

  char resolved[PATH_MAX];
  if (realpath(cand, resolved))
    snprintf(out, cap, "%s", resolved);
  else
    snprintf(out, cap, "%s", cand);
  return 1;
}

const char *rufux_freedos_dir(void) {
  static char path[PATH_MAX];
  static int bundle_inited = 0;

  const char *env = getenv("RUFUX_RES");
  if (env && env[0]) {
    char probe[PATH_MAX * 2];
    snprintf(probe, sizeof probe, "%s/freedos", env);
    if (resolve_existing_dir(probe, path, sizeof path)) return path;
  }

  if (!bundle_inited) {
    bundle_inited = 1;
    rufux_macos_init_bundle_env();
    env = getenv("RUFUX_RES");
    if (env && env[0]) {
      char probe[PATH_MAX * 2];
      snprintf(probe, sizeof probe, "%s/freedos", env);
      if (resolve_existing_dir(probe, path, sizeof path)) return path;
    }
  }

  const char *exedir = rufux_exe_dir();
  if (exedir && exedir[0]) {
    const char *rels[] = {
      "../Resources/share/rufux/freedos",
      "../../Resources/share/rufux/freedos",
      "../share/rufux/freedos",
      "../res/freedos",
      "../../res/freedos",
      "../../../res/freedos",
      NULL
    };
    for (int i = 0; rels[i]; i++) {
      char probe[PATH_MAX * 2];
      snprintf(probe, sizeof probe, "%s/%s", exedir, rels[i]);
      if (resolve_existing_dir(probe, path, sizeof path)) return path;
    }
  }

  const char *cands[] = {
    "../res/freedos",
    "res/freedos",
    "/usr/local/share/rufux/freedos",
    "/usr/share/rufux/freedos",
    NULL
  };
  for (int i = 0; cands[i]; i++) {
    if (resolve_existing_dir(cands[i], path, sizeof path)) return path;
  }
  return NULL;
}

// Pure-C byte stream copy so macOS extended attributes (e.g. com.apple.quarantine)
// are never synthesized into AppleDouble "._*" entries on FAT16/FAT32 volumes.
static int copy_regular_file(const char *src, const char *dst) {
  int sfd = open(src, O_RDONLY | O_CLOEXEC);
  if (sfd < 0) return -1;
  int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (dfd < 0) {
    close(sfd);
    return -1;
  }

  unsigned char buf[65536];
  int ok = 1;
  for (;;) {
    ssize_t r = read(sfd, buf, sizeof buf);
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) { ok = 0; break; }
    if (r == 0) break;

    size_t off = 0;
    while (off < (size_t)r) {
      ssize_t w = write(dfd, buf + off, (size_t)r - off);
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) { ok = 0; break; }
      off += (size_t)w;
    }
    if (!ok) break;
  }

  if (ok && fsync(dfd) != 0) ok = 0;
  if (close(dfd) != 0) ok = 0;
  close(sfd);
  return ok ? 0 : -1;
}

int rufux_dos_copy_files(const char *srcdir, const char *destdir,
                         char *err, unsigned long cap) {
  if (err && cap > 0) err[0] = 0;
  if (!srcdir || !srcdir[0] || !destdir || !destdir[0]) {
    if (err && cap > 0) snprintf(err, cap, "invalid FreeDOS copy directory arguments");
    return -1;
  }

  DIR *d = opendir(srcdir);
  if (!d) {
    if (err && cap > 0) snprintf(err, cap, "freedos dir '%s' missing", srcdir);
    return -1;
  }

  // KERNEL.SYS must be copied first onto the fresh FAT volume, followed by COMMAND.COM.
  const char *first[] = {"KERNEL.SYS", "COMMAND.COM", NULL};
  for (int i = 0; first[i]; i++) {
    char s[PATH_MAX * 2], t[PATH_MAX * 2];
    snprintf(s, sizeof s, "%s/%s", srcdir, first[i]);
    snprintf(t, sizeof t, "%s/%s", destdir, first[i]);
    if (access(s, R_OK) != 0) {
      if (err && cap > 0) snprintf(err, cap, "freedos payload lacks %s", first[i]);
      closedir(d);
      return -1;
    }
    if (copy_regular_file(s, t) != 0) {
      if (err && cap > 0) snprintf(err, cap, "copy failed for %s: %s", first[i], strerror(errno));
      closedir(d);
      return -1;
    }
  }

  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') continue;
    if (strcmp(e->d_name, "KERNEL.SYS") == 0 ||
        strcmp(e->d_name, "COMMAND.COM") == 0 ||
        strcasecmp(e->d_name, "readme.txt") == 0) {
      continue;
    }
    char s[PATH_MAX * 2], t[PATH_MAX * 2];
    snprintf(s, sizeof s, "%s/%s", srcdir, e->d_name);
    struct stat st;
    if (stat(s, &st) != 0 || !S_ISREG(st.st_mode)) continue;
    snprintf(t, sizeof t, "%s/%s", destdir, e->d_name);
    if (copy_regular_file(s, t) != 0) {
      if (err && cap > 0) snprintf(err, cap, "copy failed for %s: %s", e->d_name, strerror(errno));
      closedir(d);
      return -1;
    }
  }

  closedir(d);
  return 0;
}

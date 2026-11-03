#include "badblocks.h"
#include "device.h"
#include "exec.h"
#include "platform_macos.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define BB_CHUNK (1u << 20)

static int full_fsync(int fd) {
  if (fcntl(fd, F_FULLFSYNC) == 0) return 0;
  return fsync(fd);
}

int rufux_badblocks(const char *path, int allow_file, RufuxScanProgress cb, void *user,
                    unsigned long long *bad_out, char *err, unsigned long cap) {
  if (err && cap > 0) err[0] = 0;
  if (rufux_check_target(path, 1 /*scan allows fixed*/, allow_file, err, cap) != 0)
    return -1;

  struct stat st;
  if (stat(path, &st) != 0) {
    snprintf(err, cap, "cannot stat '%s': %s", path, strerror(errno));
    return -1;
  }
  int is_reg = S_ISREG(st.st_mode);
  unsigned long long total = rufux_macos_dev_bytes(path);

  char io_path[PATH_MAX];
  int fd = -1;
  if (is_reg) {
    snprintf(io_path, sizeof io_path, "%s", path);
    fd = open(io_path, O_RDONLY | O_CLOEXEC);
  } else {
    rufux_macos_raw_node(path, io_path, sizeof io_path);
    fd = open(io_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 && errno == EBUSY) {
      rufux_macos_block_node(path, io_path, sizeof io_path);
      fd = open(io_path, O_RDONLY | O_CLOEXEC);
    }
  }
  if (fd < 0) {
    snprintf(err, cap, "cannot open '%s': %s", path, strerror(errno));
    return -1;
  }

  void *vbuf = NULL;
  if (posix_memalign(&vbuf, 4096, BB_CHUNK) != 0 || !vbuf) {
    close(fd);
    snprintf(err, cap, "out of memory");
    return -1;
  }

  unsigned long long done = 0, bad = 0;
  for (;;) {
    ssize_t r = read(fd, vbuf, BB_CHUNK);
    if (r < 0) {
      if (errno == EINTR) continue;
      bad++; // short read error counts as bad region
      break;
    }
    if (r == 0) break;
    done += (unsigned long long)r;
    if (cb) cb(done, total, user);
  }

  free(vbuf);
  close(fd);
  if (cb) cb(done, total, user);
  if (bad_out) *bad_out = bad;
  return 0;
}

int rufux_badblocks_write(const char *path, int allow_file, int passes, int first,
                          RufuxScanProgress cb, void *user,
                          unsigned long long *bad_out, char *err, unsigned long cap) {
  static const unsigned char patterns[] = {0xAA, 0x55, 0xFF, 0x00};
  if (err && cap > 0) err[0] = 0;
  if (passes < 1) passes = 1;
  if (passes > 4) passes = 4;
  int first_idx = ((first % 4) + 4) % 4;

  if (rufux_check_target(path, 1, allow_file, err, cap) != 0)
    return -1;

  struct stat st;
  if (stat(path, &st) != 0) {
    snprintf(err, cap, "cannot stat '%s': %s", path, strerror(errno));
    return -1;
  }
  int is_reg = S_ISREG(st.st_mode);
  char io_path[PATH_MAX];
  if (is_reg) {
    snprintf(io_path, sizeof io_path, "%s", path);
  } else {
    if (rufux_macos_unmount_disk(path, err, cap) != 0)
      return -1;
    rufux_macos_raw_node(path, io_path, sizeof io_path);
  }

  unsigned long long total = rufux_macos_dev_bytes(io_path);
  if (total == 0) {
    snprintf(err, cap, is_reg ? "target '%s' is empty" : "cannot size '%s'", path);
    return -1;
  }

  int fd = open(io_path, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    snprintf(err, cap, "cannot open '%s': %s", io_path, strerror(errno));
    return -1;
  }

  struct stat iost;
  int io_is_chr = (!is_reg && fstat(fd, &iost) == 0 && S_ISCHR(iost.st_mode));
  unsigned sector_sz = io_is_chr ? rufux_macos_sector_size(io_path) : 1u;
  if (io_is_chr && sector_sz < 512u) sector_sz = 512u;

  size_t buf_cap = BB_CHUNK;
  if (io_is_chr && (buf_cap % sector_sz) != 0)
    buf_cap += (size_t)sector_sz - (buf_cap % sector_sz);

  void *vw = NULL, *vr = NULL;
  if (posix_memalign(&vw, 4096, buf_cap) != 0 || !vw ||
      posix_memalign(&vr, 4096, buf_cap) != 0 || !vr) {
    snprintf(err, cap, "out of memory");
    free(vw);
    free(vr);
    close(fd);
    return -1;
  }
  char *wbuf = (char *)vw;
  char *rbuf = (char *)vr;

  unsigned long long grand = 2ULL * (unsigned long long)passes * total;
  unsigned long long done = 0, bad = 0;
  int rc = 0;

  for (int p = 0; p < passes && !rc; p++) {
    unsigned char pat = patterns[(first_idx + p) % 4];
    memset(wbuf, pat, buf_cap);

    // Write phase
    if (lseek(fd, 0, SEEK_SET) != 0) {
      snprintf(err, cap, "seek failed");
      rc = -1;
      break;
    }
    unsigned long long left = total;
    while (left > 0) {
      size_t want = left > BB_CHUNK ? BB_CHUNK : (size_t)left;
      size_t io_len = want;
      if (io_is_chr && (want % sector_sz) != 0)
        io_len = want + ((size_t)sector_sz - (want % sector_sz));

      size_t off = 0;
      while (off < io_len) {
        ssize_t w = write(fd, wbuf + off, io_len - off);
        if (w <= 0) {
          if (w < 0 && errno == EINTR) continue;
          snprintf(err, cap, "write failed at pass %d: %s", p + 1, strerror(errno));
          rc = -1;
          break;
        }
        off += (size_t)w;
      }
      if (rc) break;
      left -= (unsigned long long)want;
      done += (unsigned long long)want;
      if (cb) cb(done, grand, user);
    }
    if (rc) break;
    if (full_fsync(fd) != 0) {
      snprintf(err, cap, "fsync failed");
      rc = -1;
      break;
    }
    if (!is_reg) (void)ioctl(fd, DKIOCSYNCHRONIZECACHE);

    // Verify phase
    if (lseek(fd, 0, SEEK_SET) != 0) {
      snprintf(err, cap, "seek failed");
      rc = -1;
      break;
    }
    left = total;
    while (left > 0) {
      size_t want = left > BB_CHUNK ? BB_CHUNK : (size_t)left;
      size_t io_len = want;
      if (io_is_chr && (want % sector_sz) != 0)
        io_len = want + ((size_t)sector_sz - (want % sector_sz));

      ssize_t r = rufux_read_full(fd, rbuf, io_len);
      if (r < (ssize_t)io_len) {
        bad++; // unreadable region
        break;
      }
      for (size_t i = 0; i < want; i++) {
        if ((unsigned char)rbuf[i] != pat) {
          bad++;
          break;
        }
      }
      left -= (unsigned long long)want;
      done += (unsigned long long)want;
      if (cb) cb(done, grand, user);
    }
  }

  free(vw);
  free(vr);
  close(fd);
  if (!rc && cb) cb(grand, grand, user);
  if (bad_out) *bad_out = bad;
  return rc;
}

#include "writer.h"
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
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK (1u << 20)

static int full_fsync(int fd) {
  if (fcntl(fd, F_FULLFSYNC) == 0) return 0;
  return fsync(fd);
}

// fread capped at a remaining byte budget (VHD payload support).
static size_t read_capped(FILE *f, char *buf, size_t max_chunk,
                          unsigned long long *remaining) {
  size_t want = max_chunk;
  if (*remaining < want) want = (size_t)*remaining;
  if (want == 0) return 0;
  size_t n = fread(buf, 1, want, f);
  *remaining -= n;
  return n;
}

int rufux_write_image(const char *src, const char *dst,
                      const RufuxWriteOpts *opts,
                      RufuxWriteProgress cb, void *user,
                      char *err, unsigned long errcap) {
  if (err && errcap > 0) err[0] = 0;
  if (!src || !dst || !opts) {
    if (err && errcap > 0) snprintf(err, errcap, "invalid arguments");
    return -1;
  }

  struct stat sst;
  if (stat(src, &sst) != 0 || !S_ISREG(sst.st_mode)) {
    snprintf(err, errcap, "source '%s' is not a readable file", src);
    return -1;
  }
  unsigned long long total = (unsigned long long)sst.st_size;
  if (opts->src_len > 0) {
    if (opts->src_len > total) {
      snprintf(err, errcap, "payload size exceeds file size");
      return -1;
    }
    total = opts->src_len;
  }
  if (total == 0) {
    snprintf(err, errcap, "source '%s' is empty", src);
    return -1;
  }

  if (rufux_check_target(dst, opts->allow_fixed, opts->allow_file,
                         err, errcap) != 0)
    return -1;

  // Footgun guard: source and target must not be the same file/device.
  {
    struct stat dstst;
    if (stat(dst, &dstst) == 0 && dstst.st_dev == sst.st_dev &&
        dstst.st_ino == sst.st_ino) {
      snprintf(err, errcap, "source and target are the same file ('%s')", src);
      return -1;
    }
  }

  FILE *fin = fopen(src, "rb");
  if (!fin) {
    snprintf(err, errcap, "cannot open source '%s': %s", src, strerror(errno));
    return -1;
  }

  if (opts->dry_run) {
    void *vbuf = NULL;
    if (posix_memalign(&vbuf, 4096, CHUNK) != 0 || !vbuf) {
      fclose(fin);
      snprintf(err, errcap, "out of memory");
      return -1;
    }
    unsigned long long done = 0, rem = total;
    size_t n;
    while ((n = read_capped(fin, (char *)vbuf, CHUNK, &rem)) > 0) {
      done += n;
      if (cb) cb(done, total, user);
    }
    int bad = ferror(fin);
    free(vbuf);
    fclose(fin);
    if (bad) {
      snprintf(err, errcap, "read error on '%s'", src);
      return -1;
    }
    if (cb) cb(total, total, user);
    return 0;
  }

  if (!opts->yes) {
    fclose(fin);
    snprintf(err, errcap, "refusing real write without --yes (use --dry-run to simulate)");
    return -1;
  }

  struct stat dstst;
  if (stat(dst, &dstst) != 0) {
    fclose(fin);
    snprintf(err, errcap, "target '%s' does not exist", dst);
    return -1;
  }
  int dst_is_reg = S_ISREG(dstst.st_mode);
  char io_dst[PATH_MAX];
  int fd = -1;

  if (dst_is_reg) {
    snprintf(io_dst, sizeof io_dst, "%s", dst);
    fd = open(io_dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  } else {
    if (rufux_macos_unmount_disk(dst, err, errcap) != 0) {
      fclose(fin);
      return -1;
    }
    rufux_macos_raw_node(dst, io_dst, sizeof io_dst);
    fd = open(io_dst, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
      if (flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK) {
        snprintf(err, errcap, "target '%s' is busy/locked", dst);
        close(fd);
        fclose(fin);
        return -1;
      }
    }
  }
  if (fd < 0) {
    snprintf(err, errcap, "cannot open target '%s': %s (need sudo?)",
             io_dst, strerror(errno));
    fclose(fin);
    return -1;
  }

  unsigned sector_sz = 512u;
  int io_is_chr = 0;
  if (!dst_is_reg) {
    struct stat iost;
    if (fstat(fd, &iost) == 0 && S_ISCHR(iost.st_mode)) io_is_chr = 1;
    unsigned long long dev_cap = rufux_macos_dev_bytes(io_dst);
    if (dev_cap > 0 && dev_cap < total) {
      snprintf(err, errcap, "image (%llu B) larger than target (%llu B)",
               total, dev_cap);
      close(fd);
      fclose(fin);
      return -1;
    }
    sector_sz = rufux_macos_sector_size(io_dst);
    if (sector_sz < 512u) sector_sz = 512u;
  }

  size_t buf_cap = CHUNK;
  if ((buf_cap % sector_sz) != 0)
    buf_cap += (size_t)sector_sz - (buf_cap % sector_sz);

  void *vbuf = NULL;
  if (posix_memalign(&vbuf, 4096, buf_cap) != 0 || !vbuf) {
    snprintf(err, errcap, "out of memory");
    close(fd);
    fclose(fin);
    return -1;
  }
  char *buf = (char *)vbuf;

  unsigned long long done = 0, rem = total;
  size_t n;
  int rc = 0;
  while ((n = read_capped(fin, buf, CHUNK, &rem)) > 0) {
    size_t write_len = n;
    if (io_is_chr && (n % sector_sz) != 0) {
      size_t pad = (size_t)sector_sz - (n % sector_sz);
      memset(buf + n, 0, pad);
      write_len = n + pad;
    }
    size_t off = 0;
    while (off < write_len) {
      ssize_t w = write(fd, buf + off, write_len - off);
      if (w < 0) {
        if (errno == EINTR) continue;
        snprintf(err, errcap, "write failed at %llu: %s",
                 done + off, strerror(errno));
        rc = -1;
        break;
      }
      if (w == 0) {
        snprintf(err, errcap, "write stalled at %llu", done + off);
        rc = -1;
        break;
      }
      off += (size_t)w;
    }
    if (rc != 0) break;
    done += n;
    if (cb) cb(done, total, user);
  }
  if (!rc && ferror(fin)) {
    snprintf(err, errcap, "read error on '%s'", src);
    rc = -1;
  }
  free(vbuf);
  fclose(fin);

  if (!rc && full_fsync(fd) != 0) {
    snprintf(err, errcap, "fsync failed: %s", strerror(errno));
    rc = -1;
  }
  if (!dst_is_reg) {
    (void)ioctl(fd, DKIOCSYNCHRONIZECACHE);
  }
  close(fd);
  if (cb) cb(done, total, user);

  if (!rc && opts->verify) {
    RufuxWriteProgress vcb = opts->vprog ? opts->vprog : cb;
    void *vuser = opts->vprog ? opts->vuser : user;
    FILE *fa = fopen(src, "rb");
    int fb = open(io_dst, O_RDONLY | O_CLOEXEC);
    if (!fa || fb < 0) {
      snprintf(err, errcap, "verify: cannot reopen src/dst");
      if (fa) fclose(fa);
      if (fb >= 0) close(fb);
      return -1;
    }
    void *va = NULL, *vb = NULL;
    if (posix_memalign(&va, 4096, buf_cap) != 0 || !va ||
        posix_memalign(&vb, 4096, buf_cap) != 0 || !vb) {
      snprintf(err, errcap, "out of memory");
      free(va);
      free(vb);
      fclose(fa);
      close(fb);
      return -1;
    }
    char *ba = (char *)va;
    char *bb = (char *)vb;
    unsigned long long vdone = 0, vrem = total;
    size_t na;
    lseek(fb, 0, SEEK_SET);
    while ((na = read_capped(fa, ba, CHUNK, &vrem)) > 0) {
      size_t read_len = na;
      if (io_is_chr && (na % sector_sz) != 0) {
        read_len = na + ((size_t)sector_sz - (na % sector_sz));
      }
      ssize_t rr = rufux_read_full(fb, bb, read_len);
      if (rr < 0) {
        snprintf(err, errcap, "verify: read error at offset %llu: %s",
                 vdone, strerror(errno));
        rc = -1;
        break;
      }
      size_t got = (size_t)rr;
      int ok_len = io_is_chr ? (got == read_len) : (got == na);
      if (!ok_len || memcmp(ba, bb, na) != 0) {
        snprintf(err, errcap, "verify mismatch at offset %llu", vdone);
        rc = -1;
        break;
      }
      vdone += na;
      if (vcb) vcb(vdone, total, vuser);
    }
    if (!rc && ferror(fa)) {
      snprintf(err, errcap, "verify: read error on '%s'", src);
      rc = -1;
    }
    free(va);
    free(vb);
    fclose(fa);
    close(fb);
  }
  return rc;
}

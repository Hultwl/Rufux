#define _GNU_SOURCE
#include "writer.h"
#include "exec.h"
#include "device.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#define CHUNK (1u << 20)

// fread capped at a remaining byte budget (VHD payload support).
static size_t read_capped(FILE *f, char *buf, unsigned long long *remaining) {
  size_t want = CHUNK;
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
  struct stat sst;
  if (stat(src, &sst) != 0 || !S_ISREG(sst.st_mode)) {
    snprintf(err, errcap, "source '%s' is not a readable file", src);
    return -1;
  }
  unsigned long long total = (unsigned long long)sst.st_size;
  // src_len caps the payload (fixed VHD skips its 512B footer); 0 = whole file.
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
    // simulate: read source fully, report progress, write nothing
    char *buf = malloc(CHUNK);
    unsigned long long done = 0, rem = total;
    size_t n;
    while ((n = read_capped(fin, buf, &rem)) > 0) {
      done += n;
      if (cb) cb(done, total, user);
    }
    int bad = ferror(fin);
    free(buf);
    fclose(fin);
    if (bad) { snprintf(err, errcap, "read error on '%s'", src); return -1; }
    if (cb) cb(total, total, user);
    return 0;
  }

  if (!opts->yes) {
    fclose(fin);
    snprintf(err, errcap, "refusing real write without --yes (use --dry-run to simulate)");
    return -1;
  }

  struct stat dstst;
  int dst_is_reg = (stat(dst, &dstst) == 0 && S_ISREG(dstst.st_mode));
  int fd = -1;
  if (dst_is_reg) {
    fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  } else {
    fd = open(dst, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
      // best-effort exclusive lock so two writers can't race
      if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        snprintf(err, errcap, "target '%s' is busy/locked", dst);
        close(fd); fclose(fin);
        return -1;
      }
    }
  }
  if (fd < 0) {
    snprintf(err, errcap, "cannot open target '%s': %s (need sudo?)", dst, strerror(errno));
    fclose(fin);
    return -1;
  }

  // If block device, check capacity fits
  if (!dst_is_reg) {
    unsigned long long cap = 0;
    if (ioctl(fd, BLKGETSIZE64, &cap) == 0 && cap < total) {
      snprintf(err, errcap, "image (%llu B) larger than target (%llu B)", total, cap);
      close(fd); fclose(fin);
      return -1;
    }
  }

  char *buf = malloc(CHUNK);
  if (!buf) {
    snprintf(err, errcap, "out of memory");
    close(fd); fclose(fin);
    return -1;
  }
  unsigned long long done = 0, rem = total;
  size_t n;
  int rc = 0;
  while ((n = read_capped(fin, buf, &rem)) > 0) {
    size_t off = 0;
    while (off < n) {
      ssize_t w = write(fd, buf + off, n - off);
      if (w < 0) {
        if (errno == EINTR) continue;
        snprintf(err, errcap, "write failed at %llu: %s", done + off, strerror(errno));
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
  free(buf);
  fclose(fin);
  if (fsync(fd) != 0) {
    snprintf(err, errcap, "fsync failed: %s", strerror(errno));
    rc = -1;
  }
  // revalidate + rescan partitions for block devices
  if (!rc && !dst_is_reg) {
    ioctl(fd, BLKRRPART);
  }
  if (cb) cb(done, total, user);

  if (!rc && opts->verify) {
    RufuxWriteProgress vcb = opts->vprog ? opts->vprog : cb;
    void *vuser = opts->vprog ? opts->vuser : user;
    FILE *fa = fopen(src, "rb");
    int fb = open(dst, O_RDONLY | O_CLOEXEC);
    if (!fa || fb < 0) {
      snprintf(err, errcap, "verify: cannot reopen src/dst");
      if (fa) fclose(fa);
      if (fb >= 0) close(fb);
      close(fd);
      return -1;
    }
    char *ba = malloc(CHUNK), *bb = malloc(CHUNK);
    unsigned long long vdone = 0, vrem = total;
    size_t na;
    // dst offset 0; for block, reads from start
    lseek(fb, 0, SEEK_SET);
    while ((na = read_capped(fa, ba, &vrem)) > 0) {
      ssize_t rr = rufux_read_full(fb, bb, na);
      if (rr < 0) {
        snprintf(err, errcap, "verify: read error at offset %llu: %s", vdone, strerror(errno));
        rc = -1;
        break;
      }
      size_t got = (size_t)rr;
      if (got != na || memcmp(ba, bb, na) != 0) {
        snprintf(err, errcap, "verify mismatch at offset %llu", vdone);
        rc = -1;
        break;
      }
      vdone += na;
      if (vcb) vcb(vdone, total, vuser);
    }
    free(ba); free(bb);
    fclose(fa); close(fb);
  }
  close(fd);
  return rc;
}

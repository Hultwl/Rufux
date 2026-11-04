#include "vdisk.h"
#include "device.h"
#include "exec.h"
#include "mjson.h"
#include "platform_macos.h"
#include "vhd.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// What the first bytes of the file say it is. Only files that look like a
// virtual disk are ever shown to qemu-img: ordinary ISOs and raw images never
// pass through it.
static int sniff(const char *path, char *fmt, size_t fcap, char *label, size_t lcap) {
  unsigned char h[512] = {0};
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  size_t n = fread(h, 1, sizeof h, f);
  fclose(f);
  if (n < 8) return 0;
  const char *q = NULL, *l = NULL;
  if (!memcmp(h, "QFI\xfb", 4)) { q = "qcow2"; l = "QCOW2"; }
  else if (!memcmp(h, "KDMV", 4)) { q = "vmdk"; l = "VMDK"; }
  else if (!memcmp(h, "# Disk DescriptorFile", 21)) { q = "vmdk"; l = "VMDK"; }
  else if (!memcmp(h, "vhdxfile", 8)) { q = "vhdx"; l = "VHDX"; }
  else if (n >= 0x44 && h[0x40] == 0x7f && h[0x41] == 0x10 &&
           h[0x42] == 0xda && h[0x43] == 0xbe) { q = "vdi"; l = "VDI"; }
  if (!q) return 0;
  snprintf(fmt, fcap, "%s", q);
  snprintf(label, lcap, "%s", l);
  return 1;
}

// Every file name qemu-img reports for the image must be the image itself,
// or (VMDK extents) an ordinary file in the same directory. Anything else -
// a QCOW2 backing file, an external data file, a VMDK extent pointing at
// /etc/passwd or a raw disk - would copy files the person never chose onto
// the drive.
static int refs_ok(const MJ *j, const char *self, const char *dir, char *bad, size_t bcap) {
  int t = mj_type(j);
  if (t == MJ_OBJ) {
    for (size_t i = 0; i < mj_len(j); i++) {
      const char *k = mj_key_at(j, i);
      const MJ *v = mj_at(j, i);
      if (!strcmp(k, "backing-filename") || !strcmp(k, "full-backing-filename") ||
          !strcmp(k, "data-file") || !strcmp(k, "parent-filename")) {
        const char *s = mj_str(v);
        if (s && s[0]) { snprintf(bad, bcap, "%s", s); return 0; }
      } else if (!strcmp(k, "filename") && mj_str(v)) {
        const char *s = mj_str(v);
        if (strcmp(s, self)) {
          const char *slash = strrchr(s, '/');
          size_t dl = slash ? (size_t)(slash - s) : 0;
          struct stat st;
          int same_dir = slash && strlen(dir) == dl && !strncmp(s, dir, dl);
          if (!same_dir || lstat(s, &st) != 0 || !S_ISREG(st.st_mode)) {
            snprintf(bad, bcap, "%s", s);
            return 0;
          }
        }
      }
      if (!refs_ok(v, self, dir, bad, bcap)) return 0;
    }
  } else if (t == MJ_ARR) {
    for (size_t i = 0; i < mj_len(j); i++)
      if (!refs_ok(mj_at(j, i), self, dir, bad, bcap)) return 0;
  }
  return 1;
}

int rufux_vdisk_detect(const char *src, RufuxVdisk *vd, char *err, unsigned long cap) {
  if (err && cap > 0) err[0] = 0;
  memset(vd, 0, sizeof *vd);
  struct stat st;
  if (!src || stat(src, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
  char fmt[16], label[24];
  {
    // A dynamic VHD carries a copy of its footer at offset 0 as well as at the
    // end. Cookie up front but no valid footer at the end means the file was cut short.
    unsigned char h[8] = {0};
    FILE *f = fopen(src, "rb");
    size_t n = f ? fread(h, 1, sizeof h, f) : 0;
    if (f) fclose(f);
    RufuxVhdInfo vi;
    if (n == sizeof h && !memcmp(h, "conectix", 8) &&
        rufux_vhd_probe(src, &vi) == 0 && !vi.is_vhd) {
      snprintf(err, cap,
               "'%s' looks like a dynamic VHD but its footer is missing: the file is truncated or damaged",
               src);
      return -1;
    }
  }
  if (!sniff(src, fmt, sizeof fmt, label, sizeof label)) {
    RufuxVhdInfo vi;
    if (rufux_vhd_probe(src, &vi) == 0 && vi.is_vhd && !vi.is_fixed &&
        strcmp(vi.kind, "corrupt")) {
      snprintf(fmt, sizeof fmt, "vpc");
      snprintf(label, sizeof label, "%.12s VHD", vi.kind);
    } else {
      return 0;
    }
  }
  if (!strcmp(label, "differencing VHD")) {
    snprintf(err, cap,
             "'%s' is a differencing VHD (it depends on a parent disk); merge it into a standalone image first",
             src);
    return -1;
  }
  if (!rufux_have("qemu-img")) {
    snprintf(err, cap,
             "'%s' is a %s image and needs qemu-img to be converted, which is not installed "
             "(install qemu / qemu-img)",
             src, label);
    return -1;
  }
  char real[PATH_MAX];
  if (!realpath(src, real)) {
    snprintf(err, cap, "cannot resolve '%s': %s", src, strerror(errno));
    return -1;
  }
  char dir[PATH_MAX];
  snprintf(dir, sizeof dir, "%s", real);
  char *sl = strrchr(dir, '/');
  if (sl) *sl = 0;

  const char *av[] = {"qemu-img", "info", "--output=json", "-f", fmt, real, NULL};
  char *out = malloc(1 << 20);
  if (!out) { snprintf(err, cap, "out of memory"); return -1; }
  int rc = rufux_capture(av, out, 1 << 20);
  char jerr[160] = {0};
  MJ *j = rc == 0 ? mj_parse(out, jerr, sizeof jerr) : NULL;
  if (!j) {
    char *nl = strchr(out, '\n');
    if (nl) *nl = 0;
    snprintf(err, cap, "cannot read the %s image '%s': %s", label, src,
             out[0] ? out : (jerr[0] ? jerr : "qemu-img failed"));
    free(out);
    return -1;
  }
  free(out);

  char bad[PATH_MAX] = {0};
  if (!refs_ok(j, real, dir, bad, sizeof bad)) {
    snprintf(err, cap,
             "the %s image '%s' refers to another file ('%s'); refusing to copy it onto a drive. "
             "Convert it to a standalone image first",
             label, src, bad);
    mj_free(j);
    return -1;
  }
  double vs = mj_get_num(j, "virtual-size", 0);
  const char *reported = mj_get_str(j, "format", "");
  if (vs <= 0 || strcmp(reported, fmt)) {
    snprintf(err, cap, "'%s' does not look like a valid %s image", src, label);
    mj_free(j);
    return -1;
  }
  {
    const char *ck[] = {"qemu-img", "check", "--output=json", "-f", fmt, real, NULL};
    char cout[8192];
    int crc_ = rufux_capture(ck, cout, sizeof cout);
    if (crc_ != 0 && !strstr(cout, "does not support checks")) {
      const char *pc = strstr(cout, "\"corruptions\"");
      const char *pe = strstr(cout, "\"check-errors\"");
      long corrupt = pc && strchr(pc, ':') ? atol(strchr(pc, ':') + 1) : -1;
      long errs = pe && strchr(pe, ':') ? atol(strchr(pe, ':') + 1) : -1;
      if (corrupt != 0 || errs != 0) {
        snprintf(err, cap,
                 "the %s image '%s' failed its consistency check (truncated or corrupted); "
                 "not writing it to a drive",
                 label, src);
        mj_free(j);
        return -1;
      }
    }
  }
  snprintf(vd->format, sizeof vd->format, "%s", fmt);
  snprintf(vd->label, sizeof vd->label, "%s", label);
  vd->virtual_size = (unsigned long long)vs;
  mj_free(j);
  return 1;
}

static unsigned long long target_size(const char *dst) {
  return rufux_macos_dev_bytes(dst);
}

// Runs qemu-img with stdout+stderr piped back; qemu-img -p draws "(12.34/100%)"
// with carriage returns, which are turned into progress callbacks here.
static int run_progress(const char *const av[], RufuxWriteProgress cb, void *user,
                        char *tail, size_t tcap) {
  int fd[2];
  if (pipe(fd) != 0) return -1;
  pid_t pid = fork();
  if (pid < 0) { close(fd[0]); close(fd[1]); return -1; }
  if (pid == 0) {
    dup2(fd[1], STDOUT_FILENO);
    dup2(fd[1], STDERR_FILENO);
    close(fd[0]);
    close(fd[1]);
    rufux_execvp(av[0], av);
    _exit(127);
  }
  close(fd[1]);
  char line[512];
  size_t ln = 0, tn = 0;
  if (tail && tcap) tail[0] = 0;
  char c;
  for (;;) {
    ssize_t r = read(fd[0], &c, 1);
    if (r < 0 && errno == EINTR) continue;
    if (r != 1) break;
    if (c == '\r' || c == '\n') {
      line[ln] = 0;
      double pct;
      const char *p = strchr(line, '(');
      if (p && sscanf(p, "(%lf/100%%)", &pct) == 1) {
        if (cb) cb((unsigned long long)(pct * 100.0), 10000ULL, user);
      } else if (line[0] && tail && tcap) {
        tn = strlen(tail);
        if (tn < tcap - 1)
          snprintf(tail + tn, tcap - tn, "%s%.200s", tn ? " " : "", line);
      }
      ln = 0;
    } else if (ln < sizeof line - 1) {
      line[ln++] = c;
    }
  }
  close(fd[0]);
  int st = 0;
  while (waitpid(pid, &st, 0) < 0) {
    if (errno != EINTR) return -1;
  }
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void json_escape(const char *s, char *o, size_t cap) {
  size_t n = 0;
  for (; *s && n + 3 < cap; s++) {
    if (*s == '"' || *s == '\\') o[n++] = '\\';
    o[n++] = *s;
  }
  o[n] = 0;
}

int rufux_vdisk_write(const char *src, const RufuxVdisk *vd, const char *dst,
                      const RufuxWriteOpts *opts, RufuxWriteProgress cb, void *user,
                      char *err, unsigned long cap) {
  if (err && cap > 0) err[0] = 0;
  if (rufux_check_target(dst, opts->allow_fixed, opts->allow_file, err, cap) != 0)
    return -1;
  unsigned long long tsz = target_size(dst);
  if (tsz && vd->virtual_size > tsz) {
    snprintf(err, cap,
             "the %s image is %.1f GiB once expanded, but '%s' holds only %.1f GiB",
             vd->label, vd->virtual_size / 1073741824.0, dst, tsz / 1073741824.0);
    return -1;
  }
  char real[PATH_MAX];
  if (!realpath(src, real)) {
    snprintf(err, cap, "cannot resolve '%s'", src);
    return -1;
  }
  struct stat a, b;
  if (stat(real, &a) == 0 && stat(dst, &b) == 0 &&
      a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
    snprintf(err, cap, "source and target are the same file ('%s')", src);
    return -1;
  }
  if (opts->dry_run) {
    fprintf(stderr, "+ qemu-img convert -p -f %s -O raw -n %s %s\n",
            vd->format, real, dst);
    if (cb) cb(1, 1, user);
    return 0;
  }
  if (!opts->yes) {
    snprintf(err, cap, "refusing real write without --yes (use --dry-run to simulate)");
    return -1;
  }

  struct stat dsts;
  if (stat(dst, &dsts) != 0) {
    snprintf(err, cap, "target '%s' does not exist", dst);
    return -1;
  }
  int dst_is_reg = S_ISREG(dsts.st_mode);
  char io_dst[PATH_MAX];
  if (dst_is_reg) {
    snprintf(io_dst, sizeof io_dst, "%s", dst);
    if (truncate(io_dst, (off_t)vd->virtual_size) != 0) {
      snprintf(err, cap, "cannot size '%s': %s", dst, strerror(errno));
      return -1;
    }
  } else {
    if (rufux_macos_unmount_disk(dst, err, cap) != 0)
      return -1;
    rufux_macos_raw_node(dst, io_dst, sizeof io_dst);
  }

  const char *conv[] = {"qemu-img", "convert", "-p", "-f", vd->format,
                        "-O", "raw", "-n", real, io_dst, NULL};
  char tail[512] = {0};
  int rc = run_progress(conv, cb, user, tail, sizeof tail);
  if (rc != 0) {
    snprintf(err, cap, "qemu-img could not write the %s image: %.300s",
             vd->label, tail[0] ? tail : "unknown error");
    return -1;
  }
  sync();
  if (opts->verify) {
    char esc[PATH_MAX * 2], spec[PATH_MAX * 2 + 160];
    json_escape(io_dst, esc, sizeof esc);
    struct stat ds;
    const char *drv = (stat(io_dst, &ds) == 0 &&
                       (S_ISBLK(ds.st_mode) || S_ISCHR(ds.st_mode)))
                          ? "host_device"
                          : "file";
    snprintf(spec, sizeof spec,
             "json:{\"driver\":\"raw\",\"offset\":0,\"size\":%llu,"
             "\"file\":{\"driver\":\"%s\",\"filename\":\"%s\"}}",
             vd->virtual_size, drv, esc);
    const char *cmp[] = {"qemu-img", "compare", "-f", vd->format,
                         "-F", "raw", real, spec, NULL};
    char out[512];
    int crc = rufux_capture(cmp, out, sizeof out);
    if (crc != 0) {
      snprintf(err, cap,
               "verification failed: what was written to '%s' differs from the %s image",
               dst, vd->label);
      return -1;
    }
    RufuxWriteProgress vc = opts->vprog ? opts->vprog : cb;
    if (vc) vc(1, 1, opts->vprog ? opts->vuser : user);
  }
  return 0;
}

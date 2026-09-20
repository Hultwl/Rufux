#define _GNU_SOURCE
#include "extract.h"
#include "exec.h"
#include "iso_probe.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ftw.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/statvfs.h>

// Directory content size in bytes (extraction progress polling).
// Pure libc via nftw: no du subprocess, no shell.
static unsigned long long du_acc;
static int du_cb(const char *path, const struct stat *sb, int type, struct FTW *ftw) {
  (void)path; (void)ftw;
  if (type == FTW_F) du_acc += (unsigned long long)sb->st_size;
  return 0;
}

static unsigned long long dir_size(const char *path) {
  du_acc = 0;
  struct stat st;
  if (stat(path, &st) != 0) return 0;
  if (!S_ISDIR(st.st_mode)) return (unsigned long long)st.st_size;
  nftw(path, du_cb, 16, FTW_PHYS);
  return du_acc;
}

// Run extractor in a child while the parent polls destination growth.
// total=0 disables progress (plain run). Returns extracted bytes via out.
static int extract_poll(const char *const av[], const char *dest_dir,
                        unsigned long long total,
                        RufuxExtractProgress prog, void *user,
                        unsigned long long *got_out,
                        char *err, unsigned long cap) {
  unsigned long long base = dir_size(dest_dir);
  pid_t pid = fork();
  if (pid < 0) { snprintf(err, cap, "fork failed"); return -1; }
  if (pid == 0) {
    execvp(av[0], (char *const *)av);
    _exit(127);
  }
  int rc = 0;
  for (;;) {
    int st = 0;
    pid_t w = waitpid(pid, &st, WNOHANG);
    if (w < 0) { rc = -1; break; }
    if (w == pid) {
      rc = (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
      break;
    }
    if (prog && total) {
      unsigned long long cur = dir_size(dest_dir);
      prog(cur > base ? cur - base : 0, total, user);
    }
    usleep(200000);
  }
  if (prog && total && rc == 0) {
    unsigned long long cur = dir_size(dest_dir);
    prog(cur > base ? cur - base : 0, total, user);
  }
  if (got_out) {
    unsigned long long cur = dir_size(dest_dir);
    *got_out = cur > base ? cur - base : 0;
  }
  if (rc != 0) snprintf(err, cap, "%s extract failed", av[0]);
  return rc;
}

int rufux_extract_iso_progress(const char *src, const char *dest_dir, int dry_run,
                               RufuxExtractProgress prog, void *user,
                               char *err, unsigned long cap) {
  struct stat st;
  if (stat(src, &st) != 0) { snprintf(err, cap, "source '%s' missing", src); return -1; }
  unsigned long long total = S_ISREG(st.st_mode) ? (unsigned long long)st.st_size : 0;
  if (!dry_run) {
    if (mkdir(dest_dir, 0755) != 0 && errno != EEXIST) {
      snprintf(err, cap, "cannot mkdir '%s': %s", dest_dir, strerror(errno));
      return -1;
    }
  }
  // Backend routing: UDF images go straight to 7z (bsdtar silently
  // under-extracts some UDF layouts, e.g. Win11 media). Others prefer
  // bsdtar with a 7z fallback.
  int want_7z = rufux_iso_is_udf(src) > 0;
  int have_bsdtar = rufux_have("bsdtar");
  int rc = 0;
  unsigned long long got = 0;
  if (rufux_have("bsdtar") && !want_7z) {
    const char *av[] = {"bsdtar", "-xf", src, "-C", dest_dir, NULL};
    if (dry_run) { rufux_run(av, 1); return 0; }
    rc = extract_poll(av, dest_dir, (prog && total) ? total : 0, prog, user,
                      &got, err, cap);
    // bsdtar can exit 0 while under-extracting some UDF layouts: fall
    // through to 7z instead of declaring success on a partial tree.
    if (rc != 0 || (total > (50ULL << 20) && got * 4 < total)) {
      if (rc == 0 && rufux_have("7z")) {
        snprintf(err, cap, "bsdtar incomplete (%llu of %llu bytes), retrying with 7z",
                 got, total);
        have_bsdtar = 0; // force the 7z path below (keep err if it fails too)
      } else {
        if (rc != 0) return rc;
        snprintf(err, cap, "extract incomplete (%llu of %llu bytes) and no 7z available",
                 got, total);
        return -1;
      }
    } else {
      return rc;
    }
  }
  if (rufux_have("7z")) {
    char out[1152];
    snprintf(out, sizeof out, "-o%s", dest_dir);
    const char *av[] = {"7z", "x", src, "-y", out, NULL};
    if (dry_run) { rufux_run(av, 1); return 0; }
    rc = extract_poll(av, dest_dir, (prog && total) ? total : 0, prog, user,
                      &got, err, cap);
    if (rc == 0 && total > (50ULL << 20) && got * 4 < total) {
      snprintf(err, cap, "extract incomplete (%llu of %llu bytes)", got, total);
      return -1;
    }
    return rc;
  }
  if (!have_bsdtar && !rufux_have("7z"))
    snprintf(err, cap, "need bsdtar or 7z for extraction (7z required for UDF)");
  return -1;
}

int rufux_extract_iso(const char *src, const char *dest_dir, int dry_run,
                      char *err, unsigned long cap) {
  return rufux_extract_iso_progress(src, dest_dir, dry_run, NULL, NULL, err, cap);
}

int rufux_write_autorun(const char *dir, const char *label, int dry_run,
                        char *err, unsigned long cap) {
  if (!label || !label[0]) label = "RUFUX";
  char path[1024];
  snprintf(path, sizeof path, "%s/autorun.inf", dir);
  if (dry_run) {
    fprintf(stderr, "+ write %s (label '%s')\n", path, label);
    return 0;
  }
  FILE *f = fopen(path, "w");
  if (!f) { snprintf(err, cap, "cannot write '%s': %s", path, strerror(errno)); return -1; }
  fprintf(f, "[Autorun]\nLabel=%s\n", label);
  if (fclose(f) != 0) { snprintf(err, cap, "cannot close '%s'", path); return -1; }
  return 0;
}

// ---------------------------------------------------------------------------
// FAT32 media: files must stay under 4 GiB, but Windows 11 install.wim is often
// larger. Everything except the WIM is extracted normally; the WIM is
// extracted to scratch space and split into install.swm, install2.swm, ...
// with wimlib. Windows Setup reads split WIMs natively.
// ---------------------------------------------------------------------------
static unsigned long long archive_file_size(const char *iso, const char *name) {
  char out[8192] = {0};
  const char *av[] = {"7z", "l", "-slt", iso, name, NULL};
  if (rufux_capture(av, out, sizeof out) != 0) return 0;
  const char *p = strstr(out, "\nSize = ");
  return p ? strtoull(p + 8, NULL, 10) : 0;
}

typedef struct {
  RufuxExtractProgress prog;
  void *user;
  unsigned long long base, grand;
} SplitMap;

static void split_cb(unsigned long long done, unsigned long long total, void *u) {
  (void)total;
  SplitMap *m = (SplitMap *)u;
  if (m->prog) m->prog(m->base + done, m->grand, m->user);
}

// A directory with room for `need` bytes: RUFUX_TMPDIR, TMPDIR, /var/tmp, /tmp.
static int scratch_dir(unsigned long long need, char *out, size_t cap) {
  const char *cand[] = {getenv("RUFUX_TMPDIR"), getenv("TMPDIR"), "/var/tmp", "/tmp", NULL};
  for (int i = 0; cand[i]; i++) {
    if (!cand[i][0]) continue;
    struct statvfs vs;
    if (statvfs(cand[i], &vs) != 0) continue;
    unsigned long long avail = (unsigned long long)vs.f_bavail * vs.f_frsize;
    if (avail < need) continue;
    snprintf(out, cap, "%s/rufux-wim-XXXXXX", cand[i]);
    if (mkdtemp(out)) return 0;
  }
  return -1;
}

int rufux_extract_windows_split(const char *src, const char *dest_dir, unsigned split_mb, int force,
                                RufuxExtractProgress prog, void *user, RufuxExtractLog log,
                                void *log_user, char *err, unsigned long cap) {
  static const unsigned long long FAT_MAX = 4294967295ULL;
  struct stat st;
  if (stat(src, &st) != 0) { snprintf(err, cap, "source '%s' missing", src); return -1; }
  unsigned long long total = (unsigned long long)st.st_size;
  if (!rufux_have("7z")) { snprintf(err, cap, "7z is required to read Windows ISOs"); return -1; }
  unsigned long long wim = archive_file_size(src, "sources/install.wim");
  unsigned long long esd = archive_file_size(src, "sources/install.esd");
  if (esd > FAT_MAX) {
    snprintf(err, cap, "install.esd is %.1f GiB: too large for FAT32 and it cannot be split. Use NTFS.",
             esd / 1073741824.0);
    return -1;
  }
  if (!force && wim <= FAT_MAX) {  // everything fits: plain extraction
    if (log) log("install.wim fits on FAT32 (no splitting needed).", log_user);
    return rufux_extract_iso_progress(src, dest_dir, 0, prog, user, err, cap);
  }
  if (!wim) { snprintf(err, cap, "no sources/install.wim in the image to split"); return -1; }
  if (!rufux_have("wimlib-imagex")) {
    snprintf(err, cap,
             "install.wim is %.1f GiB, too large for FAT32. Install wimlib (wimlib-imagex) so Rufux can "
             "split it, or choose NTFS.", wim / 1073741824.0);
    return -1;
  }
  char msg[300];
  snprintf(msg, sizeof msg, "Splitting install.wim (%.1f GiB) into %u MiB parts for FAT32...",
           wim / 1073741824.0, split_mb);
  if (log) log(msg, log_user);

  if (mkdir(dest_dir, 0755) != 0 && errno != EEXIST) {
    snprintf(err, cap, "cannot mkdir '%s': %s", dest_dir, strerror(errno));
    return -1;
  }
  // Phase A: the whole image minus the WIM.
  SplitMap ma = {prog, user, 0, total};
  char out[1152];
  snprintf(out, sizeof out, "-o%s", dest_dir);
  const char *xa[] = {"7z", "x", src, "-y", out, "-x!sources/install.wim", NULL};
  unsigned long long got = 0;
  if (extract_poll(xa, dest_dir, total > wim ? total - wim : 0, split_cb, &ma, &got, err, cap) != 0) return -1;

  // Phase B: the WIM to scratch space, then split it onto the stick.
  char scratch[512];
  if (scratch_dir(wim + (256ULL << 20), scratch, sizeof scratch) != 0) {
    snprintf(err, cap,
             "no scratch space: %.1f GiB free needed in /var/tmp or /tmp (set RUFUX_TMPDIR to a bigger disk)",
             (wim + (256ULL << 20)) / 1073741824.0);
    return -1;
  }
  int rc = 0;
  SplitMap mb = {prog, user, total > wim ? total - wim : 0, total};
  char so[1300];
  snprintf(so, sizeof so, "-o%s", scratch);
  const char *xb[] = {"7z", "e", src, "-y", so, "sources/install.wim", NULL};
  if (extract_poll(xb, scratch, wim, split_cb, &mb, &got, err, cap) != 0) rc = -1;
  if (!rc) {
    char sdir[1300], wimfile[1300], part[1400], mb_s[16];
    snprintf(sdir, sizeof sdir, "%s/sources", dest_dir);
    if (mkdir(sdir, 0755) != 0 && errno != EEXIST) {
      snprintf(err, cap, "cannot mkdir '%s': %s", sdir, strerror(errno));
      rc = -1;
    } else {
      snprintf(wimfile, sizeof wimfile, "%s/install.wim", scratch);
      snprintf(part, sizeof part, "%s/install.swm", sdir);
      snprintf(mb_s, sizeof mb_s, "%u", split_mb);
      const char *sp[] = {"wimlib-imagex", "split", wimfile, part, mb_s, NULL};
      if (rufux_run(sp, 0) != 0) { snprintf(err, cap, "wimlib-imagex split failed"); rc = -1; }
    }
  }
  const char *rm[] = {"rm", "-rf", "--", scratch, NULL};
  rufux_run(rm, 0);
  if (!rc && prog) prog(total, total, user);
  return rc;
}

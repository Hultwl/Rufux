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
#include <dirent.h>
#include <strings.h>

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
  // A complete extraction lands within a few percent of the image size, so
  // anything under three quarters of it means files were dropped. The old
  // threshold was a quarter, which let an ISO that lost most of sources/
  // through unnoticed - the stick then booted and Windows Setup blamed a
  // missing media driver.
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
    if (rc != 0 || (total > (50ULL << 20) && got * 4 < total * 3)) {
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
    if (rc == 0 && total > (50ULL << 20) && got * 4 < total * 3) {
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

// A directory with room for `need` bytes: RUFUX_TMPDIR, TMPDIR, the
// directory the image itself lives in (usually a roomy disk), /var/tmp, /tmp.
static int scratch_dir(unsigned long long need, const char *src, char *out, size_t cap) {
  char srcdir[512] = {0};
  if (src) {
    snprintf(srcdir, sizeof srcdir, "%s", src);
    char *slash = strrchr(srcdir, '/');
    if (slash && slash != srcdir) *slash = 0; else srcdir[0] = 0;
  }
  const char *cand[] = {getenv("RUFUX_TMPDIR"), getenv("TMPDIR"),
                        srcdir[0] ? srcdir : NULL, "/var/tmp", "/tmp", NULL};
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

// Loop-mount the image read-only. wimlib can then read install.wim in place,
// which is what makes the scratch copy unnecessary.
static int iso_mount_ro(const char *iso, char *mnt, size_t cap) {
  snprintf(mnt, cap, "/tmp/rufux-iso-XXXXXX");
  if (!mkdtemp(mnt)) return -1;
  const char *av[] = {"mount", "-o", "loop,ro", "--", iso, mnt, NULL};
  if (rufux_run(av, 0) != 0) { rmdir(mnt); return -1; }
  return 0;
}

static void iso_umount(const char *mnt) {
  const char *av[] = {"umount", "--", mnt, NULL};
  rufux_run(av, 0);
  rmdir(mnt);
}

// Windows images carry the same tree twice, once per descriptor, and the two
// disagree on case, so resolve each component by comparing names ignoring it.
static int resolve_ci(const char *root, const char *rel, char *out, size_t cap) {
  char cur[1024];
  snprintf(cur, sizeof cur, "%s", root);
  char rest[256];
  snprintf(rest, sizeof rest, "%s", rel);
  char *save = NULL;
  for (char *seg = strtok_r(rest, "/", &save); seg; seg = strtok_r(NULL, "/", &save)) {
    DIR *d = opendir(cur);
    if (!d) return -1;
    struct dirent *e;
    char hit[256] = {0};
    while ((e = readdir(d)) != NULL)
      if (!strcasecmp(e->d_name, seg)) { snprintf(hit, sizeof hit, "%s", e->d_name); break; }
    closedir(d);
    if (!hit[0]) return -1;
    size_t L = strlen(cur);
    snprintf(cur + L, sizeof cur - L, "/%s", hit);
  }
  if (access(cur, R_OK) != 0) return -1;
  snprintf(out, cap, "%s", cur);
  return 0;
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

  // Phase B: split the WIM onto the stick.
  //
  // Preferred route: loop-mount the image read-only and let wimlib read
  // install.wim where it lies. Extracting it to scratch first needed as much
  // free space as the WIM (6.6 GiB for a Windows 11 image), which is why this
  // step used to fail on machines with a small /tmp or /var/tmp.
  char sdir[1300], wimfile[1300], part[1400], mb_s[16];
  snprintf(sdir, sizeof sdir, "%s/sources", dest_dir);
  if (mkdir(sdir, 0755) != 0 && errno != EEXIST) {
    snprintf(err, cap, "cannot mkdir '%s': %s", sdir, strerror(errno));
    return -1;
  }
  snprintf(part, sizeof part, "%s/install.swm", sdir);
  snprintf(mb_s, sizeof mb_s, "%u", split_mb);

  int rc = -1;
  char isomnt[256] = {0};
  if (iso_mount_ro(src, isomnt, sizeof isomnt) == 0) {
    if (resolve_ci(isomnt, "sources/install.wim", wimfile, sizeof wimfile) == 0) {
      if (log) log("Splitting install.wim directly from the mounted image.", log_user);
      const char *sp[] = {"wimlib-imagex", "split", wimfile, part, mb_s, NULL};
      rc = rufux_run(sp, 0) == 0 ? 0 : -1;
      if (rc != 0) snprintf(err, cap, "wimlib-imagex split failed");
    }
    iso_umount(isomnt);
  }

  if (rc != 0) {
    // Fallback for images that will not loop-mount: copy the WIM out first.
    char scratch[512];
    if (scratch_dir(wim + (256ULL << 20), src, scratch, sizeof scratch) != 0) {
      snprintf(err, cap,
               "cannot split install.wim: the image would not mount, and there is no scratch "
               "space for a copy (%.1f GiB free needed; set RUFUX_TMPDIR to a bigger disk)",
               (wim + (256ULL << 20)) / 1073741824.0);
      return -1;
    }
    SplitMap mb = {prog, user, total > wim ? total - wim : 0, total};
    char so[1300];
    snprintf(so, sizeof so, "-o%s", scratch);
    const char *xb[] = {"7z", "e", src, "-y", so, "sources/install.wim", NULL};
    if (extract_poll(xb, scratch, wim, split_cb, &mb, &got, err, cap) == 0) {
      snprintf(wimfile, sizeof wimfile, "%s/install.wim", scratch);
      const char *sp[] = {"wimlib-imagex", "split", wimfile, part, mb_s, NULL};
      rc = rufux_run(sp, 0) == 0 ? 0 : -1;
      if (rc != 0) snprintf(err, cap, "wimlib-imagex split failed");
    }
    const char *rm[] = {"rm", "-rf", "--", scratch, NULL};
    rufux_run(rm, 0);
  }
  if (!rc && prog) prog(total, total, user);
  return rc;
}

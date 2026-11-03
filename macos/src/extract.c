#include "extract.h"
#include "exec.h"
#include "iso_probe.h"
#include "platform_macos.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static _Thread_local unsigned long long du_acc;

static int du_cb(const char *path, const struct stat *sb, int type, struct FTW *ftw) {
  (void)path;
  (void)ftw;
  if (type == FTW_F) du_acc += (unsigned long long)sb->st_size;
  return 0;
}

static unsigned long long dir_size(const char *path) {
  du_acc = 0;
  struct stat st;
  if (!path || stat(path, &st) != 0) return 0;
  if (!S_ISDIR(st.st_mode)) return (unsigned long long)st.st_size;
  nftw(path, du_cb, 16, FTW_PHYS);
  return du_acc;
}

static int extract_poll(const char *const av[], const char *dest_dir,
                        unsigned long long total,
                        RufuxExtractProgress prog, void *user,
                        unsigned long long *got_out,
                        char *err, unsigned long cap) {
  unsigned long long base = dir_size(dest_dir);
  pid_t pid = fork();
  if (pid < 0) {
    if (err && cap > 0) snprintf(err, cap, "fork failed");
    return -1;
  }
  if (pid == 0) {
    int devnull = -1;
    // Redirect noisy 7zz/bsdtar stdout to /dev/null while leaving stderr visible.
    if ((devnull = open("/dev/null", O_WRONLY)) >= 0) {
      dup2(devnull, STDOUT_FILENO);
      close(devnull);
    }
    rufux_execvp(av[0], av);
    _exit(127);
  }
  int rc = 0;
  for (;;) {
    int st = 0;
    pid_t w = waitpid(pid, &st, WNOHANG);
    if (w < 0) {
      if (errno == EINTR) continue;
      rc = -1;
      break;
    }
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
  if (rc != 0 && err && cap > 0) snprintf(err, cap, "%s extract failed", av[0]);
  return rc;
}

int rufux_extract_iso_progress(const char *src, const char *dest_dir, int dry_run,
                               RufuxExtractProgress prog, void *user,
                               char *err, unsigned long cap) {
  if (err && cap > 0) err[0] = 0;
  struct stat st;
  if (!src || stat(src, &st) != 0) {
    if (err && cap > 0) snprintf(err, cap, "source '%s' missing", src ? src : "");
    return -1;
  }
  unsigned long long total = S_ISREG(st.st_mode) ? (unsigned long long)st.st_size : 0;
  if (!dry_run) {
    if (mkdir(dest_dir, 0755) != 0 && errno != EEXIST) {
      if (err && cap > 0) snprintf(err, cap, "cannot mkdir '%s': %s", dest_dir, strerror(errno));
      return -1;
    }
  }

  int want_7z = rufux_iso_is_udf(src) > 0;
  int have_bsdtar = rufux_have("bsdtar");
  int rc = 0;
  unsigned long long got = 0;
  if (have_bsdtar && !want_7z) {
    const char *av[] = {"bsdtar", "-xf", src, "-C", dest_dir, NULL};
    if (dry_run) {
      rufux_run(av, 1);
      return 0;
    }
    rc = extract_poll(av, dest_dir, (prog && total) ? total : 0, prog, user,
                      &got, err, cap);
    if (rc != 0 || (total > (50ULL << 20) && got * 4 < total * 3)) {
      if (rc == 0 && rufux_have("7z")) {
        if (err && cap > 0)
          snprintf(err, cap, "bsdtar incomplete (%llu of %llu bytes), retrying with 7z",
                   got, total);
        have_bsdtar = 0;
      } else {
        if (rc != 0) return rc;
        if (err && cap > 0)
          snprintf(err, cap, "extract incomplete (%llu of %llu bytes) and no 7z available",
                   got, total);
        return -1;
      }
    } else {
      return rc;
    }
  }
  if (rufux_have("7z")) {
    char out[PATH_MAX + 8];
    snprintf(out, sizeof out, "-o%s", dest_dir);
    const char *av[] = {"7z", "x", src, "-y", out, NULL};
    if (dry_run) {
      rufux_run(av, 1);
      return 0;
    }
    rc = extract_poll(av, dest_dir, (prog && total) ? total : 0, prog, user,
                      &got, err, cap);
    if (rc == 0 && total > (50ULL << 20) && got * 4 < total * 3) {
      if (err && cap > 0)
        snprintf(err, cap, "extract incomplete (%llu of %llu bytes)", got, total);
      return -1;
    }
    return rc;
  }
  if (!have_bsdtar && !rufux_have("7z") && err && cap > 0)
    snprintf(err, cap, "need bsdtar or 7z for extraction (7z required for UDF)");
  return -1;
}

int rufux_extract_iso(const char *src, const char *dest_dir, int dry_run,
                      char *err, unsigned long cap) {
  return rufux_extract_iso_progress(src, dest_dir, dry_run, NULL, NULL, err, cap);
}

int rufux_write_autorun(const char *dir, const char *label, int dry_run,
                        char *err, unsigned long cap) {
  if (err && cap > 0) err[0] = 0;
  if (!label || !label[0]) label = "RUFUX";
  char path[PATH_MAX];
  snprintf(path, sizeof path, "%s/autorun.inf", dir);
  if (dry_run) {
    fprintf(stderr, "+ write %s (label '%s')\n", path, label);
    return 0;
  }
  FILE *f = fopen(path, "w");
  if (!f) {
    if (err && cap > 0) snprintf(err, cap, "cannot write '%s': %s", path, strerror(errno));
    return -1;
  }
  fprintf(f, "[Autorun]\nLabel=%s\n", label);
  if (fclose(f) != 0) {
    if (err && cap > 0) snprintf(err, cap, "cannot close '%s'", path);
    return -1;
  }
  return 0;
}

static int path_eq_ci_norm(const char *a, const char *b) {
  while (*a && *b) {
    char ca = (*a == '\\') ? '/' : (char)tolower((unsigned char)*a);
    char cb = (*b == '\\') ? '/' : (char)tolower((unsigned char)*b);
    if (ca != cb) return 0;
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

static int parse_slt_for_entry(char *text, const char *relpath_ci,
                               char *actual_out, unsigned long path_cap,
                               unsigned long long *size_out) {
  char cur_path[512] = {0};
  int have_path = 0;
  int have_size = 0;
  unsigned long long cur_size = 0;
  int in_entries = 0;

  char *save = NULL;
  for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    size_t L = strlen(line);
    while (L > 0 && (line[L - 1] == '\r' || line[L - 1] == ' ')) line[--L] = 0;
    if (strcmp(line, "----------") == 0) {
      in_entries = 1;
      cur_path[0] = 0;
      have_path = 0;
      have_size = 0;
      cur_size = 0;
      continue;
    }
    if (!in_entries) continue;
    if (strncmp(line, "Path = ", 7) == 0) {
      if (have_path && have_size && path_eq_ci_norm(cur_path, relpath_ci)) {
        if (actual_out && path_cap > 0) snprintf(actual_out, path_cap, "%s", cur_path);
        if (size_out) *size_out = cur_size;
        return 1;
      }
      snprintf(cur_path, sizeof cur_path, "%s", line + 7);
      have_path = 1;
      have_size = 0;
      cur_size = 0;
    } else if (strncmp(line, "Size = ", 7) == 0 && isdigit((unsigned char)line[7])) {
      cur_size = strtoull(line + 7, NULL, 10);
      have_size = 1;
    }
  }
  if (have_path && have_size && path_eq_ci_norm(cur_path, relpath_ci)) {
    if (actual_out && path_cap > 0) snprintf(actual_out, path_cap, "%s", cur_path);
    if (size_out) *size_out = cur_size;
    return 1;
  }
  return 0;
}

int rufux_archive_find_entry(const char *iso, const char *relpath_ci,
                             char *actual_path_out, unsigned long path_cap,
                             unsigned long long *size_out) {
  if (actual_path_out && path_cap > 0) actual_path_out[0] = 0;
  if (size_out) *size_out = 0;
  if (!iso || !relpath_ci || !rufux_have("7z")) return 0;

  char upper[256] = {0};
  for (size_t i = 0; relpath_ci[i] && i + 1 < sizeof upper; i++)
    upper[i] = (char)toupper((unsigned char)relpath_ci[i]);

  const char *tries[2] = {relpath_ci, upper};
  for (int t = 0; t < 2; t++) {
    if (t == 1 && strcmp(tries[0], tries[1]) == 0) continue;
    char small_out[16384] = {0};
    const char *av[] = {"7z", "l", "-slt", iso, tries[t], NULL};
    if (rufux_capture(av, small_out, sizeof small_out) == 0 &&
        parse_slt_for_entry(small_out, relpath_ci, actual_path_out, path_cap, size_out)) {
      return 1;
    }
  }

  const unsigned long big_cap = 2UL * 1024UL * 1024UL;
  char *big = malloc(big_cap);
  if (!big) return 0;
  big[0] = 0;
  const char *av_all[] = {"7z", "l", "-slt", iso, NULL};
  int found = 0;
  if (rufux_capture(av_all, big, big_cap) == 0) {
    found = parse_slt_for_entry(big, relpath_ci, actual_path_out, path_cap, size_out);
  }
  free(big);
  return found;
}

int rufux_extract_iso_excluding(const char *src, const char *dest_dir,
                                const char *exclude_entry,
                                unsigned long long expect_bytes,
                                RufuxExtractProgress prog, void *user,
                                char *err, unsigned long cap) {
  if (err && cap > 0) err[0] = 0;
  if (!rufux_have("7z")) {
    if (err && cap > 0) snprintf(err, cap, "7z is required to extract ISO images");
    return -1;
  }
  if (mkdir(dest_dir, 0755) != 0 && errno != EEXIST) {
    if (err && cap > 0) snprintf(err, cap, "cannot mkdir '%s': %s", dest_dir, strerror(errno));
    return -1;
  }
  char out[PATH_MAX + 8];
  snprintf(out, sizeof out, "-o%s", dest_dir);
  unsigned long long got = 0;
  if (exclude_entry && exclude_entry[0]) {
    char xarg[540];
    snprintf(xarg, sizeof xarg, "-x!%s", exclude_entry);
    const char *av[] = {"7z", "x", src, "-y", out, xarg, NULL};
    return extract_poll(av, dest_dir, prog ? expect_bytes : 0, prog, user, &got, err, cap);
  }
  const char *av[] = {"7z", "x", src, "-y", out, NULL};
  return extract_poll(av, dest_dir, prog ? expect_bytes : 0, prog, user, &got, err, cap);
}

int rufux_extract_single_entry(const char *src, const char *dest_dir,
                               const char *entry_path,
                               unsigned long long expect_bytes,
                               RufuxExtractProgress prog, void *user,
                               char *err, unsigned long cap) {
  if (err && cap > 0) err[0] = 0;
  if (!rufux_have("7z") || !entry_path || !entry_path[0]) {
    if (err && cap > 0) snprintf(err, cap, "7z is required to extract archive entry");
    return -1;
  }
  if (mkdir(dest_dir, 0755) != 0 && errno != EEXIST) {
    if (err && cap > 0) snprintf(err, cap, "cannot mkdir '%s': %s", dest_dir, strerror(errno));
    return -1;
  }
  char out[PATH_MAX + 8];
  snprintf(out, sizeof out, "-o%s", dest_dir);
  const char *av[] = {"7z", "e", src, "-y", out, entry_path, NULL};
  unsigned long long got = 0;
  return extract_poll(av, dest_dir, prog ? expect_bytes : 0, prog, user, &got, err, cap);
}

int rufux_scratch_dir(unsigned long long need, const char *src,
                      char *out, unsigned long cap) {
  char srcdir[PATH_MAX] = {0};
  if (src) {
    snprintf(srcdir, sizeof srcdir, "%s", src);
    char *slash = strrchr(srcdir, '/');
    if (slash && slash != srcdir) *slash = 0;
    else srcdir[0] = 0;
  }
  const char *cand[] = {
    getenv("RUFUX_TMPDIR"),
    getenv("TMPDIR"),
    srcdir[0] ? srcdir : NULL,
    "/var/tmp",
    "/tmp"
  };
  for (size_t i = 0; i < sizeof(cand) / sizeof(cand[0]); i++) {
    if (!cand[i] || !cand[i][0]) continue;
    struct statvfs vs;
    if (statvfs(cand[i], &vs) != 0) continue;
    unsigned long long avail = (unsigned long long)vs.f_bavail * (unsigned long long)vs.f_frsize;
    if (avail < need) continue;
    size_t clen = strlen(cand[i]);
    while (clen > 1 && cand[i][clen - 1] == '/') clen--;
    if (clen + 20 >= cap) continue;
    snprintf(out, cap, "%.*s/rufux-work-XXXXXX", (int)clen, cand[i]);
    if (mkdtemp(out)) return 0;
  }
  return -1;
}

static int parse_first_whole_disk(const char *hdi_out, char *disk_out, unsigned long cap) {
  if (!disk_out || cap == 0) return -1;
  disk_out[0] = 0;
  for (const char *p = hdi_out; (p = strstr(p, "/dev/disk")) != NULL; p += 9) {
    if (p > hdi_out && !isspace((unsigned char)p[-1])) continue;
    const char *q = p + 9;
    if (!isdigit((unsigned char)*q)) continue;
    while (isdigit((unsigned char)*q)) q++;
    if (*q == '\0' || isspace((unsigned char)*q)) {
      size_t len = (size_t)(q - p);
      if (len + 1 > cap) return -1;
      memcpy(disk_out, p, len);
      disk_out[len] = 0;
      return 0;
    }
  }
  return -1;
}

int rufux_iso_mount_ro(const char *iso, char *mnt_out, unsigned long mnt_cap,
                       char *dev_out, unsigned long dev_cap) {
  if (!iso || !mnt_out || mnt_cap < 32 || !dev_out || dev_cap < 32) return -1;
  mnt_out[0] = 0;
  dev_out[0] = 0;

  snprintf(mnt_out, mnt_cap, "/tmp/rufux-iso-XXXXXX");
  if (!mkdtemp(mnt_out)) {
    mnt_out[0] = 0;
    return -1;
  }

  char hout[4096] = {0};
  const char *av1[] = {"/usr/bin/hdiutil", "attach", "-readonly", "-nomount", iso, NULL};
  int attached = (rufux_capture(av1, hout, sizeof hout) == 0 &&
                  parse_first_whole_disk(hout, dev_out, dev_cap) == 0);
  if (!attached) {
    const char *av2[] = {
      "/usr/bin/hdiutil", "attach",
      "-imagekey", "diskimage-class=CRawDiskImage",
      "-readonly", "-nomount", iso, NULL
    };
    attached = (rufux_capture(av2, hout, sizeof hout) == 0 &&
                parse_first_whole_disk(hout, dev_out, dev_cap) == 0);
  }
  if (!attached) {
    rmdir(mnt_out);
    mnt_out[0] = 0;
    dev_out[0] = 0;
    return -1;
  }

  char slice1[64], slice0[64];
  snprintf(slice1, sizeof slice1, "%ss1", dev_out);
  snprintf(slice0, sizeof slice0, "%ss0", dev_out);
  const char *devs[] = {dev_out, slice1, slice0, NULL};
  const char *fstypes[] = {"udf", "cd9660", NULL};

  // Prefer UDF first when UDF anchor is present, otherwise cd9660 first.
  int is_udf = rufux_iso_is_udf(iso) > 0;
  if (!is_udf) {
    fstypes[0] = "cd9660";
    fstypes[1] = "udf";
  }

  char mout[512];
  for (int d = 0; devs[d]; d++) {
    if (d > 0 && access(devs[d], F_OK) != 0) continue;
    for (int f = 0; fstypes[f]; f++) {
      const char *mav[] = {
        "/sbin/mount", "-r", "-o", "nobrowse", "-t", fstypes[f], devs[d], mnt_out, NULL
      };
      if (rufux_capture(mav, mout, sizeof mout) == 0) return 0;
    }
  }

  (void)rufux_macos_detach_disk(dev_out);
  rmdir(mnt_out);
  mnt_out[0] = 0;
  dev_out[0] = 0;
  return -1;
}

void rufux_iso_umount_ro(const char *mnt, const char *dev) {
  char out[512];
  if (mnt && mnt[0]) {
    const char *u1[] = {"/sbin/umount", mnt, NULL};
    if (rufux_capture(u1, out, sizeof out) != 0) {
      usleep(100000);
      const char *u2[] = {"/sbin/umount", "-f", mnt, NULL};
      (void)rufux_capture(u2, out, sizeof out);
    }
    rmdir(mnt);
  }
  if (dev && dev[0]) {
    (void)rufux_macos_detach_disk(dev);
  }
}

static int resolve_ci(const char *root, const char *rel, char *out, size_t cap) {
  char cur[PATH_MAX];
  snprintf(cur, sizeof cur, "%s", root);
  char rest[512];
  snprintf(rest, sizeof rest, "%s", rel);
  char *save = NULL;
  for (char *seg = strtok_r(rest, "/", &save); seg; seg = strtok_r(NULL, "/", &save)) {
    DIR *d = opendir(cur);
    if (!d) return -1;
    struct dirent *e;
    char hit[256] = {0};
    while ((e = readdir(d)) != NULL) {
      if (!strcasecmp(e->d_name, seg)) {
        snprintf(hit, sizeof hit, "%s", e->d_name);
        break;
      }
    }
    closedir(d);
    if (!hit[0]) return -1;
    size_t L = strlen(cur);
    snprintf(cur + L, sizeof cur - L, "/%s", hit);
  }
  if (access(cur, R_OK) != 0) return -1;
  snprintf(out, cap, "%s", cur);
  return 0;
}

static int ensure_subdir_ci(const char *root, const char *name, char *out, size_t cap) {
  DIR *d = opendir(root);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      if (!strcasecmp(e->d_name, name)) {
        snprintf(out, cap, "%s/%s", root, e->d_name);
        closedir(d);
        return 0;
      }
    }
    closedir(d);
  }
  snprintf(out, cap, "%s/%s", root, name);
  if (mkdir(out, 0755) != 0 && errno != EEXIST) return -1;
  return 0;
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

int rufux_extract_windows_split(const char *src, const char *dest_dir,
                                unsigned split_mb, int force,
                                RufuxExtractProgress prog, void *user,
                                RufuxExtractLog log, void *log_user,
                                char *err, unsigned long cap) {
  static const unsigned long long FAT_MAX = 4294967295ULL;
  if (err && cap > 0) err[0] = 0;

  struct stat st;
  if (!src || stat(src, &st) != 0) {
    if (err && cap > 0) snprintf(err, cap, "source '%s' missing", src ? src : "");
    return -1;
  }
  unsigned long long total = (unsigned long long)st.st_size;
  if (!rufux_have("7z")) {
    if (err && cap > 0) snprintf(err, cap, "7z is required to read Windows ISOs");
    return -1;
  }

  char wim_entry[256] = {0}, esd_entry[256] = {0};
  unsigned long long wim = 0, esd = 0;
  (void)rufux_archive_find_entry(src, "sources/install.wim", wim_entry, sizeof wim_entry, &wim);
  (void)rufux_archive_find_entry(src, "sources/install.esd", esd_entry, sizeof esd_entry, &esd);

  if (esd > FAT_MAX) {
    if (err && cap > 0)
      snprintf(err, cap, "install.esd is %.1f GiB: too large for FAT32 and it cannot be split. Use NTFS.",
               esd / 1073741824.0);
    return -1;
  }
  if (!force && wim <= FAT_MAX) {
    if (log) log("install.wim fits on FAT32 (no splitting needed).", log_user);
    return rufux_extract_iso_progress(src, dest_dir, 0, prog, user, err, cap);
  }
  if (!wim || !wim_entry[0]) {
    if (err && cap > 0) snprintf(err, cap, "no sources/install.wim in the image to split");
    return -1;
  }
  if (!rufux_have("wimlib-imagex")) {
    if (err && cap > 0)
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
    if (err && cap > 0) snprintf(err, cap, "cannot mkdir '%s': %s", dest_dir, strerror(errno));
    return -1;
  }

  // Phase A: extract everything except sources/install.wim.
  SplitMap ma = {prog, user, 0, total};
  if (rufux_extract_iso_excluding(src, dest_dir, wim_entry,
                                  total > wim ? total - wim : 0,
                                  split_cb, &ma, err, cap) != 0) {
    return -1;
  }

  // Phase B: split install.wim into install.swm, install2.swm, ...
  char sdir[PATH_MAX], wimfile[PATH_MAX], part[PATH_MAX], mb_s[16];
  if (ensure_subdir_ci(dest_dir, "sources", sdir, sizeof sdir) != 0) {
    if (err && cap > 0) snprintf(err, cap, "cannot mkdir '%s': %s", sdir, strerror(errno));
    return -1;
  }
  snprintf(part, sizeof part, "%s/install.swm", sdir);
  snprintf(mb_s, sizeof mb_s, "%u", split_mb);

  int rc = -1;
  char isomnt[256] = {0}, isodev[128] = {0};
  if (rufux_iso_mount_ro(src, isomnt, sizeof isomnt, isodev, sizeof isodev) == 0) {
    struct stat mst;
    struct statfs sfs;
    int is_cd9660_over_4g = (wim > FAT_MAX && statfs(isomnt, &sfs) == 0 &&
                             strcmp(sfs.f_fstypename, "cd9660") == 0);
    if (!is_cd9660_over_4g &&
        resolve_ci(isomnt, "sources/install.wim", wimfile, sizeof wimfile) == 0 &&
        stat(wimfile, &mst) == 0 && mst.st_size > 0 &&
        (unsigned long long)mst.st_size == wim) {
      if (log) log("Splitting install.wim directly from the mounted image.", log_user);
      const char *sp[] = {"wimlib-imagex", "split", wimfile, part, mb_s, NULL};
      rc = rufux_run(sp, 0) == 0 ? 0 : -1;
      if (rc != 0 && err && cap > 0) snprintf(err, cap, "wimlib-imagex split failed");
    }
    rufux_iso_umount_ro(isomnt, isodev);
  }

  if (rc != 0) {
    char scratch[PATH_MAX];
    if (rufux_scratch_dir(wim + (256ULL << 20), src, scratch, sizeof scratch) != 0) {
      if (err && cap > 0)
        snprintf(err, cap,
                 "cannot split install.wim: the image would not mount, and there is no scratch "
                 "space for a copy (%.1f GiB free needed; set RUFUX_TMPDIR to a bigger disk)",
                 (wim + (256ULL << 20)) / 1073741824.0);
      return -1;
    }
    SplitMap mb = {prog, user, total > wim ? total - wim : 0, total};
    if (rufux_extract_single_entry(src, scratch, wim_entry, wim, split_cb, &mb, err, cap) == 0) {
      struct stat sst;
      if (resolve_ci(scratch, "install.wim", wimfile, sizeof wimfile) == 0 &&
          stat(wimfile, &sst) == 0 && (unsigned long long)sst.st_size == wim) {
        const char *sp[] = {"wimlib-imagex", "split", wimfile, part, mb_s, NULL};
        rc = rufux_run(sp, 0) == 0 ? 0 : -1;
        if (rc != 0 && err && cap > 0) snprintf(err, cap, "wimlib-imagex split failed");
      } else if (err && cap > 0) {
        snprintf(err, cap, "extracted install.wim missing or size mismatch in scratch dir");
      }
    }
    const char *rm[] = {"/bin/rm", "-rf", scratch, NULL};
    (void)rufux_run(rm, 0);
  }

  if (!rc && prog) prog(total, total, user);
  return rc;
}

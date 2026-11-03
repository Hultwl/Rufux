#include "wininstall.h"
#include "exec.h"
#include "platform_macos.h"
#include <CoreFoundation/CoreFoundation.h>
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

#define UEFI_NTFS_IMG_URL "https://raw.githubusercontent.com/pbatard/rufus/master/res/uefi/uefi-ntfs.img"
#define UEFI_NTFS_IMG_SIZE 1048576UL

static int mkdir_p(const char *path) {
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof tmp, "%s", path);
  for (char *c = tmp + 1; *c; c++) {
    if (*c == '/') {
      *c = 0;
      mkdir(tmp, 0755);
      *c = '/';
    }
  }
  if (mkdir(path, 0755) != 0 && errno != EEXIST) return -1;
  return 0;
}

static int cache_dir(char *out, unsigned long cap) {
  const char *base = getenv("XDG_CACHE_HOME");
  if (base && base[0]) {
    snprintf(out, cap, "%s/rufux", base);
  } else {
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = "/tmp";
    snprintf(out, cap, "%s/Library/Caches/rufux", home);
  }
  if (mkdir_p(out) != 0) {
    snprintf(out, cap, "/tmp/rufux-cache-%u", (unsigned)geteuid());
    if (mkdir_p(out) != 0) return -1;
  }
  struct stat st;
  return (stat(out, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;
}

static int resolve_existing_img(const char *cand, char *out, size_t cap) {
  if (!cand || !cand[0]) return 0;
  struct stat st;
  if (stat(cand, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 100000) return 0;
  char resolved[PATH_MAX];
  if (realpath(cand, resolved)) snprintf(out, cap, "%s", resolved);
  else snprintf(out, cap, "%s", cand);
  return 1;
}

static const char *uefi_img_local(void) {
  static char path[PATH_MAX];
  static int bundle_inited = 0;

  const char *env = getenv("RUFUX_RES");
  if (env && env[0]) {
    char p1[PATH_MAX * 2], p2[PATH_MAX * 2];
    snprintf(p1, sizeof p1, "%s/uefi/uefi-ntfs.img", env);
    snprintf(p2, sizeof p2, "%s/uefi-ntfs.img", env);
    if (resolve_existing_img(p1, path, sizeof path)) return path;
    if (resolve_existing_img(p2, path, sizeof path)) return path;
  }

  if (!bundle_inited) {
    bundle_inited = 1;
    rufux_macos_init_bundle_env();
    env = getenv("RUFUX_RES");
    if (env && env[0]) {
      char p1[PATH_MAX * 2], p2[PATH_MAX * 2];
      snprintf(p1, sizeof p1, "%s/uefi/uefi-ntfs.img", env);
      snprintf(p2, sizeof p2, "%s/uefi-ntfs.img", env);
      if (resolve_existing_img(p1, path, sizeof path)) return path;
      if (resolve_existing_img(p2, path, sizeof path)) return path;
    }
  }

  const char *exedir = rufux_exe_dir();
  if (exedir && exedir[0]) {
    const char *rels[] = {
      "../Resources/share/rufux/uefi-ntfs.img",
      "../Resources/share/rufux/uefi/uefi-ntfs.img",
      "../../Resources/share/rufux/uefi-ntfs.img",
      "../../Resources/share/rufux/uefi/uefi-ntfs.img",
      "../share/rufux/uefi-ntfs.img",
      "../share/rufux/uefi/uefi-ntfs.img",
      "../res/uefi/uefi-ntfs.img",
      "../../res/uefi/uefi-ntfs.img",
      "../../../res/uefi/uefi-ntfs.img",
      NULL
    };
    for (int i = 0; rels[i]; i++) {
      char probe[PATH_MAX * 2];
      snprintf(probe, sizeof probe, "%s/%s", exedir, rels[i]);
      if (resolve_existing_img(probe, path, sizeof path)) return path;
    }
  }

  static const char *cands[] = {
    "../res/uefi/uefi-ntfs.img",
    "res/uefi/uefi-ntfs.img",
    "/usr/local/share/rufux/uefi-ntfs.img",
    "/usr/share/rufux/uefi-ntfs.img",
    NULL,
  };
  for (int i = 0; cands[i]; i++) {
    if (resolve_existing_img(cands[i], path, sizeof path)) return path;
  }
  return NULL;
}

static int uefi_img_path(char *out, unsigned long cap, char *err, unsigned long errcap) {
  const char *local = uefi_img_local();
  if (local) {
    snprintf(out, cap, "%s", local);
    return 0;
  }
  char dir[PATH_MAX];
  if (cache_dir(dir, sizeof dir) != 0) {
    snprintf(err, errcap, "cannot create cache dir");
    return -1;
  }
  snprintf(out, cap, "%s/uefi-ntfs.img", dir);
  struct stat st;
  if (stat(out, &st) == 0 && (unsigned long)st.st_size >= UEFI_NTFS_IMG_SIZE) return 0;
  if (!rufux_have("curl")) {
    snprintf(err, errcap, "UEFI:NTFS image not found locally and no curl to fetch it (offline?)");
    return -1;
  }
  const char *av[] = {"curl", "-sL", "--max-time", "60", "-o", out, UEFI_NTFS_IMG_URL, NULL};
  if (rufux_run(av, 0) != 0 || stat(out, &st) != 0 ||
      (unsigned long)st.st_size < UEFI_NTFS_IMG_SIZE) {
    snprintf(err, errcap, "download of UEFI:NTFS image failed (offline?)");
    return -1;
  }
  return 0;
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
  if (access(cur, F_OK) != 0) return -1;
  snprintf(out, cap, "%s", cur);
  return 0;
}

static int copy_dir_flat_files(const char *src_dir, const char *dst_dir) {
  DIR *d = opendir(src_dir);
  if (!d) return -1;
  struct dirent *e;
  unsigned char buf[65536];
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') continue;
    char sp[PATH_MAX], dp[PATH_MAX];
    snprintf(sp, sizeof sp, "%s/%s", src_dir, e->d_name);
    struct stat st;
    if (stat(sp, &st) != 0 || !S_ISREG(st.st_mode)) continue;
    snprintf(dp, sizeof dp, "%s/%s", dst_dir, e->d_name);
    int sfd = open(sp, O_RDONLY | O_CLOEXEC);
    if (sfd < 0) { closedir(d); return -1; }
    int dfd = open(dp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (dfd < 0) { close(sfd); closedir(d); return -1; }
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
    close(dfd);
    close(sfd);
    if (!ok) { closedir(d); return -1; }
  }
  closedir(d);
  return 0;
}

int rufux_stage_uefi_ntfs(const char *tmpdir, char *err, unsigned long cap) {
  if (err && cap > 0) err[0] = 0;
  char img[PATH_MAX];
  if (uefi_img_path(img, sizeof img, err, cap) != 0) return -1;
  if (!rufux_have("7z")) {
    snprintf(err, cap, "need 7z to unpack the UEFI:NTFS image");
    return -1;
  }
  char bootd[PATH_MAX], rufusd[PATH_MAX], xtr[PATH_MAX];
  snprintf(bootd, sizeof bootd, "%s/esp/EFI/BOOT", tmpdir);
  snprintf(rufusd, sizeof rufusd, "%s/esp/EFI/Rufus", tmpdir);
  snprintf(xtr, sizeof xtr, "%s/.uefintfs", tmpdir);
  if (mkdir_p(bootd) != 0 || mkdir_p(rufusd) != 0 || mkdir_p(xtr) != 0) {
    snprintf(err, cap, "cannot mkdir ESP staging dirs");
    return -1;
  }

  char out[PATH_MAX + 8];
  snprintf(out, sizeof out, "-o%s", xtr);
  const char *av[] = {"7z", "x", "-y", img, out, NULL};
  if (rufux_run(av, 0) != 0) {
    snprintf(err, cap, "cannot extract UEFI:NTFS image");
    return -1;
  }

  char s1[PATH_MAX], s2[PATH_MAX];
  int copied = (resolve_ci(xtr, "EFI/Boot", s1, sizeof s1) == 0 &&
                resolve_ci(xtr, "EFI/Rufus", s2, sizeof s2) == 0 &&
                copy_dir_flat_files(s1, bootd) == 0 &&
                copy_dir_flat_files(s2, rufusd) == 0);

  const char *rmv[] = {"/bin/rm", "-rf", xtr, NULL};
  (void)rufux_run(rmv, 0);
  if (!copied) {
    snprintf(err, cap, "cannot stage UEFI:NTFS tree");
    return -1;
  }

  char boot[PATH_MAX], drv[PATH_MAX];
  struct stat bst, dst;
  if (resolve_ci(tmpdir, "esp/EFI/BOOT/bootx64.efi", boot, sizeof boot) != 0 ||
      stat(boot, &bst) != 0 || bst.st_size < 10000) {
    snprintf(err, cap, "staged bootloader missing or too small");
    return -1;
  }
  if (resolve_ci(tmpdir, "esp/EFI/Rufus/ntfs_x64.efi", drv, sizeof drv) != 0 ||
      stat(drv, &dst) != 0 || dst.st_size < 10000) {
    snprintf(err, cap, "staged NTFS driver missing (EFI/Rufus/ntfs_x64.efi)");
    return -1;
  }
  return 0;
}

int rufux_write_uefi_ntfs(const char *part_dev, char *err, unsigned long cap) {
  if (err && cap > 0) err[0] = 0;
  if (!part_dev || !part_dev[0]) {
    snprintf(err, cap, "empty UEFI:NTFS partition path");
    return -1;
  }
  if (rufux_macos_is_boot_disk(part_dev)) {
    snprintf(err, cap, "refusing boot/system disk '%s'", part_dev);
    return -1;
  }

  char img[PATH_MAX];
  if (uefi_img_path(img, sizeof img, err, cap) != 0) return -1;

  FILE *in = fopen(img, "rb");
  if (!in) {
    snprintf(err, cap, "cannot open '%s': %s", img, strerror(errno));
    return -1;
  }
  unsigned char *want = malloc(UEFI_NTFS_IMG_SIZE);
  unsigned char *got = malloc(UEFI_NTFS_IMG_SIZE);
  if (!want || !got) {
    fclose(in);
    free(want);
    free(got);
    snprintf(err, cap, "out of memory");
    return -1;
  }
  size_t n = fread(want, 1, UEFI_NTFS_IMG_SIZE, in);
  fclose(in);
  if (n != UEFI_NTFS_IMG_SIZE) {
    snprintf(err, cap, "UEFI:NTFS image '%s' is %zu bytes, expected %lu", img, n, UEFI_NTFS_IMG_SIZE);
    free(want);
    free(got);
    return -1;
  }

  char bnode[PATH_MAX];
  rufux_macos_block_node(part_dev, bnode, sizeof bnode);
  unsigned long long psz = rufux_macos_dev_bytes(bnode);
  if (psz < UEFI_NTFS_IMG_SIZE) {
    snprintf(err, cap, "partition '%s' (%llu bytes) is smaller than the UEFI:NTFS image", part_dev, psz);
    free(want);
    free(got);
    return -1;
  }

  struct stat st;
  int is_dev = (stat(bnode, &st) == 0 && (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)));
  int fd = open(bnode, O_RDWR | O_CLOEXEC);
  if (fd < 0 && errno == EBUSY && is_dev) {
    char uout[512];
    const char *uav[] = {"/usr/sbin/diskutil", "unmount", "force", bnode, NULL};
    (void)rufux_capture(uav, uout, sizeof uout);
    usleep(100000);
    fd = open(bnode, O_RDWR | O_CLOEXEC);
  }
  if (fd < 0) {
    snprintf(err, cap, "cannot open '%s': %s", bnode, strerror(errno));
    free(want);
    free(got);
    return -1;
  }

  (void)fcntl(fd, F_NOCACHE, 1);
  if (rufux_pwrite_all(fd, want, UEFI_NTFS_IMG_SIZE, 0) != 0) {
    snprintf(err, cap, "writing UEFI:NTFS image failed: %s", strerror(errno));
    close(fd);
    free(want);
    free(got);
    return -1;
  }
  if (fsync(fd) != 0) {
    snprintf(err, cap, "fsync of '%s' failed: %s", bnode, strerror(errno));
    close(fd);
    free(want);
    free(got);
    return -1;
  }
  close(fd);

  // Reopen with F_NOCACHE so read-back bypasses macOS UBC cache.
  int rfd = open(bnode, O_RDONLY | O_CLOEXEC);
  if (rfd < 0) {
    snprintf(err, cap, "cannot reopen '%s' for verify: %s", bnode, strerror(errno));
    free(want);
    free(got);
    return -1;
  }
  (void)fcntl(rfd, F_NOCACHE, 1);
  size_t off = 0;
  while (off < UEFI_NTFS_IMG_SIZE) {
    ssize_t r = pread(rfd, got + off, UEFI_NTFS_IMG_SIZE - off, (off_t)off);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    off += (size_t)r;
  }
  close(rfd);

  int ok = (off == UEFI_NTFS_IMG_SIZE && memcmp(want, got, UEFI_NTFS_IMG_SIZE) == 0);
  free(want);
  free(got);
  if (!ok) {
    snprintf(err, cap, "UEFI:NTFS image read-back mismatch on '%s'", part_dev);
    return -1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Windows User Experience (WUE), Rufus style, adapted for macOS.
// ---------------------------------------------------------------------------
typedef struct {
  int bypass, nro, privacy, bitlocker, locale, qol;
  char user[64];
  char lang[32];
  char kbd[32];
  char tz[64];
} Wue;

static const char *const bad_names[] = {
  "administrator", "guest", "defaultaccount", "wdagutilityaccount", "system",
  "network", "local service", "network service", "krbtgt", "root", NULL
};

static int wue_parse(const char *spec, Wue *w, char *err, unsigned long cap) {
  memset(w, 0, sizeof *w);
  if (spec && !strcmp(spec, "none")) return 1;
  if (!spec || !spec[0]) { w->bypass = 1; return 0; }
  char tmp[512];
  snprintf(tmp, sizeof tmp, "%s", spec);
  for (char *save = NULL, *tok = strtok_r(tmp, ",", &save); tok;
       tok = strtok_r(NULL, ",", &save)) {
    while (*tok == ' ') tok++;
    if (!strncasecmp(tok, "user=", 5)) {
      snprintf(w->user, sizeof w->user, "%s", tok + 5);
      for (char *c = w->user; *c; c++)
        if (strchr("\"/\\[]:;|=,+*?<>@&", *c) || (unsigned char)*c < 32) *c = '_';
      for (int i = 0; bad_names[i]; i++) {
        if (!strcasecmp(w->user, bad_names[i])) {
          snprintf(err, cap, "'%s' is not allowed as a local account name", w->user);
          return -1;
        }
      }
      continue;
    }
    if (!strcasecmp(tok, "bypass")) w->bypass = 1;
    else if (!strcasecmp(tok, "nro")) w->nro = 1;
    else if (!strcasecmp(tok, "privacy")) w->privacy = 1;
    else if (!strcasecmp(tok, "bitlocker")) w->bitlocker = 1;
    else if (!strcasecmp(tok, "locale")) w->locale = 1;
    else if (!strcasecmp(tok, "qol")) w->qol = 1;
    else if (!strcasecmp(tok, "all")) w->bypass = w->nro = w->privacy = w->bitlocker = w->qol = 1;
    else {
      snprintf(err, cap, "unknown --wue item '%s' (bypass,nro,privacy,bitlocker,locale,qol,user=NAME,all)", tok);
      return -1;
    }
  }
  if (!w->bypass && !w->nro && !w->privacy && !w->bitlocker && !w->locale && !w->qol && !w->user[0]) {
    snprintf(err, cap, "--wue names no option");
    return -1;
  }
  return 0;
}

static int resolve_ci_win(const char *root, const char *rel, char *out, size_t cap) {
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
  if (access(cur, F_OK) != 0) return -1;
  snprintf(out, cap, "%s", cur);
  return 0;
}

static int ensure_child_dir_ci(const char *parent, const char *name, char *out, size_t cap) {
  DIR *d = opendir(parent);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      if (!strcasecmp(e->d_name, name)) {
        snprintf(out, cap, "%s/%s", parent, e->d_name);
        closedir(d);
        return 0;
      }
    }
    closedir(d);
  }
  snprintf(out, cap, "%s/%s", parent, name);
  if (mkdir(out, 0755) != 0 && errno != EEXIST) return -1;
  return 0;
}

static const char *media_arch(const char *root) {
  char p[PATH_MAX];
  if (resolve_ci_win(root, "efi/boot/bootaa64.efi", p, sizeof p) == 0) return "arm64";
  if (resolve_ci_win(root, "efi/boot/bootx64.efi", p, sizeof p) == 0) return "amd64";
  if (resolve_ci_win(root, "efi/boot/bootia32.efi", p, sizeof p) == 0) return "x86";
  return "amd64";
}

static const struct { const char *iana, *win; } tz_map[] = {
  {"UTC", "UTC"}, {"Etc/UTC", "UTC"}, {"Africa/Cairo", "Egypt Standard Time"},
  {"Africa/Johannesburg", "South Africa Standard Time"}, {"Africa/Lagos", "W. Central Africa Standard Time"},
  {"Africa/Nairobi", "E. Africa Standard Time"}, {"Africa/Casablanca", "Morocco Standard Time"},
  {"Africa/Algiers", "W. Central Africa Standard Time"}, {"Africa/Tunis", "W. Central Africa Standard Time"},
  {"Europe/London", "GMT Standard Time"}, {"Europe/Dublin", "GMT Standard Time"},
  {"Europe/Lisbon", "GMT Standard Time"}, {"Europe/Paris", "Romance Standard Time"},
  {"Europe/Madrid", "Romance Standard Time"}, {"Europe/Brussels", "Romance Standard Time"},
  {"Europe/Berlin", "W. Europe Standard Time"}, {"Europe/Amsterdam", "W. Europe Standard Time"},
  {"Europe/Rome", "W. Europe Standard Time"}, {"Europe/Vienna", "W. Europe Standard Time"},
  {"Europe/Zurich", "W. Europe Standard Time"}, {"Europe/Stockholm", "W. Europe Standard Time"},
  {"Europe/Oslo", "W. Europe Standard Time"}, {"Europe/Warsaw", "Central European Standard Time"},
  {"Europe/Prague", "Central Europe Standard Time"}, {"Europe/Budapest", "Central Europe Standard Time"},
  {"Europe/Athens", "GTB Standard Time"}, {"Europe/Bucharest", "GTB Standard Time"},
  {"Europe/Helsinki", "FLE Standard Time"}, {"Europe/Sofia", "FLE Standard Time"},
  {"Europe/Kiev", "FLE Standard Time"}, {"Europe/Kyiv", "FLE Standard Time"},
  {"Europe/Istanbul", "Turkey Standard Time"}, {"Europe/Moscow", "Russian Standard Time"},
  {"Asia/Riyadh", "Arab Standard Time"}, {"Asia/Dubai", "Arabian Standard Time"},
  {"Asia/Baghdad", "Arabic Standard Time"}, {"Asia/Jerusalem", "Israel Standard Time"},
  {"Asia/Tehran", "Iran Standard Time"}, {"Asia/Kolkata", "India Standard Time"},
  {"Asia/Karachi", "Pakistan Standard Time"}, {"Asia/Dhaka", "Bangladesh Standard Time"},
  {"Asia/Bangkok", "SE Asia Standard Time"}, {"Asia/Shanghai", "China Standard Time"},
  {"Asia/Hong_Kong", "China Standard Time"}, {"Asia/Singapore", "Singapore Standard Time"},
  {"Asia/Tokyo", "Tokyo Standard Time"}, {"Asia/Seoul", "Korea Standard Time"},
  {"Australia/Sydney", "AUS Eastern Standard Time"}, {"Pacific/Auckland", "New Zealand Standard Time"},
  {"America/New_York", "Eastern Standard Time"}, {"America/Toronto", "Eastern Standard Time"},
  {"America/Chicago", "Central Standard Time"}, {"America/Denver", "Mountain Standard Time"},
  {"America/Los_Angeles", "Pacific Standard Time"}, {"America/Sao_Paulo", "E. South America Standard Time"},
  {"America/Argentina/Buenos_Aires", "Argentina Standard Time"},
  {"America/Mexico_City", "Central Standard Time (Mexico)"}, {NULL, NULL}
};

static const struct { const char *tag, *klid; } kbd_map[] = {
  {"en-US", "0409:00000409"}, {"en-GB", "0809:00000809"}, {"en", "0409:00000409"},
  {"ar", "0401:00000401"}, {"fr-CA", "0c0c:00001009"}, {"fr", "040c:0000040c"},
  {"de", "0407:00000407"}, {"es-MX", "080a:0000080a"}, {"es", "0c0a:0000040a"},
  {"it", "0410:00000410"}, {"pt-BR", "0416:00000416"}, {"pt", "0816:00000816"},
  {"ru", "0419:00000419"}, {"tr", "041f:0000041f"}, {"nl", "0413:00020409"},
  {"pl", "0415:00000415"}, {"sv", "041d:0000041d"}, {"da", "0406:00000406"},
  {"nb", "0414:00000414"}, {"fi", "040b:0000040b"}, {"cs", "0405:00000405"},
  {"el", "0408:00000408"}, {"he", "040d:0000040d"}, {"ja", "0411:00000411"},
  {"ko", "0412:00000412"}, {"zh-CN", "0804:00000804"}, {"zh", "0404:00000404"},
  {"hi", "0439:00010439"}, {"fa", "0429:00000429"}, {"uk", "0422:00020422"},
  {NULL, NULL}
};

static void lang_tag(const char *in, char *out, size_t cap) {
  int valid = (in && in[0] && strcmp(in, "C") && strcmp(in, "POSIX") &&
               strncmp(in, "C.", 2) && strncmp(in, "UTF-8", 5));
  snprintf(out, cap, "%s", valid ? in : "en-US");
  char *d = strpbrk(out, ".@");
  if (d) *d = 0;
  for (char *c = out; *c; c++) if (*c == '_') *c = '-';
}

static const char *klid_for(const char *tag) {
  for (int i = 0; kbd_map[i].tag; i++)
    if (!strcasecmp(kbd_map[i].tag, tag)) return kbd_map[i].klid;
  char pri[8] = {0};
  snprintf(pri, sizeof pri, "%s", tag);
  char *d = strchr(pri, '-');
  if (d) *d = 0;
  for (int i = 0; kbd_map[i].tag; i++)
    if (!strcasecmp(kbd_map[i].tag, pri)) return kbd_map[i].klid;
  return "0409:00000409";
}

static void tz_lookup(const char *zone, char *out, size_t cap) {
  out[0] = 0;
  if (!zone || !zone[0]) return;
  if (strstr(zone, "Standard Time") || !strcmp(zone, "UTC")) {
    snprintf(out, cap, "%s", zone);
    return;
  }
  for (int i = 0; tz_map[i].iana; i++) {
    if (!strcmp(tz_map[i].iana, zone)) {
      snprintf(out, cap, "%s", tz_map[i].win);
      return;
    }
  }
}

static void macos_detect_lang(char *out, size_t cap) {
  const char *env = getenv("LC_ALL");
  if (!env || !env[0]) env = getenv("LANG");
  if (env && env[0] && strcmp(env, "C") && strcmp(env, "POSIX") &&
      strncmp(env, "C.", 2) && strncmp(env, "UTF-8", 5)) {
    lang_tag(env, out, cap);
    return;
  }
  CFArrayRef langs = CFLocaleCopyPreferredLanguages();
  if (langs) {
    if (CFArrayGetCount(langs) > 0) {
      CFStringRef s = (CFStringRef)CFArrayGetValueAtIndex(langs, 0);
      char buf[64] = {0};
      if (s && CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8) && buf[0]) {
        CFRelease(langs);
        lang_tag(buf, out, cap);
        return;
      }
    }
    CFRelease(langs);
  }
  snprintf(out, cap, "en-US");
}

static void macos_detect_tz(char *zone, size_t cap) {
  zone[0] = 0;
  char l[PATH_MAX];
  ssize_t n = readlink("/etc/localtime", l, sizeof l - 1);
  if (n > 0) {
    l[n] = 0;
    const char *z = strstr(l, "zoneinfo/");
    if (z) {
      snprintf(zone, cap, "%s", z + 9);
      return;
    }
  }
  CFTimeZoneRef tz = CFTimeZoneCopySystem();
  if (tz) {
    CFStringRef name = CFTimeZoneGetName(tz);
    if (name) (void)CFStringGetCString(name, zone, (CFIndex)cap, kCFStringEncodingUTF8);
    CFRelease(tz);
  }
}

static void locale_fill(Wue *w, const char *lang, const char *kbd, const char *tz) {
  if (lang && lang[0]) lang_tag(lang, w->lang, sizeof w->lang);
  else macos_detect_lang(w->lang, sizeof w->lang);
  snprintf(w->kbd, sizeof w->kbd, "%s", (kbd && kbd[0]) ? kbd : klid_for(w->lang));
  char zone[128] = {0};
  if (tz && tz[0]) snprintf(zone, sizeof zone, "%s", tz);
  else macos_detect_tz(zone, sizeof zone);
  tz_lookup(zone, w->tz, sizeof w->tz);
}

static void xesc(FILE *f, const char *s) {
  for (; *s; s++) {
    switch (*s) {
      case '&': fputs("&amp;", f); break;
      case '<': fputs("&lt;", f); break;
      case '>': fputs("&gt;", f); break;
      default: fputc(*s, f);
    }
  }
}

static int first_logon(const Wue *w, const char *out[], size_t max, char *bufs, size_t bufcap) {
  int n = 0;
  size_t used = 0;
  if (w->user[0]) {
    int len = snprintf(bufs + used, bufcap - used, "net user \"%s\" /logonpasswordchg:yes", w->user);
    out[n++] = bufs + used;
    used += (size_t)len + 1;
    out[n++] = "net accounts /maxpwage:unlimited";
  }
  if (w->qol) {
    static const char *q[] = {
      "reg add \"HKLM\\System\\CurrentControlSet\\Control\\Session Manager\\Power\" /v HiberbootEnabled /t REG_DWORD /d 0 /f",
      "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\" /v ShowCopilotButton /t REG_DWORD /d 0 /f",
      "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\WindowsCopilot\" /v TurnOffWindowsCopilot /t REG_DWORD /d 1 /f",
      "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Search\" /v SearchboxTaskbarMode /t REG_DWORD /d 1 /f",
      "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\CloudContent\" /v DisableWindowsConsumerFeatures /t REG_DWORD /d 1 /f",
      "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager\" /v SystemPaneSuggestionsEnabled /t REG_DWORD /d 0 /f",
      "reg add \"HKCU\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Search\" /v BingSearchEnabled /t REG_DWORD /d 0 /f",
      "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\Device Metadata\" /v PreventDeviceMetadataFromNetwork /t REG_DWORD /d 1 /f",
      "reg add \"HKLM\\Software\\Policies\\Microsoft\\Dsh\" /v AllowNewsAndInterests /t REG_DWORD /d 0 /f",
      "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\Windows Feeds\" /v EnableFeeds /t REG_DWORD /d 0 /f",
      "reg add \"HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Communications\" /v ConfigureChatAutoInstall /t REG_DWORD /d 0 /f",
      "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\CloudContent\" /v DisableCloudOptimizedContent /t REG_DWORD /d 1 /f",
      "reg add \"HKLM\\Software\\Policies\\Microsoft\\Edge\" /v HideFirstRunExperience /t REG_DWORD /d 1 /f",
      "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\" /v Start_Layout /t REG_DWORD /d 1 /f",
      "reg add \"HKCU\\Software\\Classes\\CLSID\\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\\InprocServer32\" /ve /t REG_SZ /d \"\" /f",
      NULL
    };
    for (int i = 0; q[i] && (size_t)n < max; i++) out[n++] = q[i];
  }
  return n;
}

static int write_xml(const char *path, const char *arch, const Wue *w, int winpe_bypass,
                     char *err, unsigned long cap) {
  FILE *f = fopen(path, "w");
  if (!f) {
    snprintf(err, cap, "cannot write '%s': %s", path, strerror(errno));
    return -1;
  }
  char comp[512];
  snprintf(comp, sizeof comp, "processorArchitecture=\"%s\" publicKeyToken=\"31bf3856ad364e35\" "
           "language=\"neutral\" versionScope=\"nonSxS\" "
           "xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" "
           "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"", arch);
  fprintf(f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
             "<!-- Generated by Rufux (Windows User Experience). -->\n"
             "<unattend xmlns=\"urn:schemas-microsoft-com:unattend\">\n");
  if (winpe_bypass) {
    static const char *keys[] = {"BypassTPMCheck", "BypassSecureBootCheck", "BypassRAMCheck", NULL};
    fprintf(f, "  <settings pass=\"windowsPE\">\n"
               "    <component name=\"Microsoft-Windows-Setup\" %s>\n"
               "      <UserData>\n        <AcceptEula>true</AcceptEula>\n"
               "        <ProductKey>\n          <Key />\n        </ProductKey>\n      </UserData>\n"
               "      <RunSynchronous>\n", comp);
    for (int i = 0; keys[i]; i++)
      fprintf(f, "        <RunSynchronousCommand wcm:action=\"add\">\n"
                 "          <Order>%d</Order>\n"
                 "          <Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig /v %s /t REG_DWORD /d 1 /f</Path>\n"
                 "        </RunSynchronousCommand>\n", i + 1, keys[i]);
    fprintf(f, "      </RunSynchronous>\n    </component>\n  </settings>\n");
  }
  if (w->nro)
    fprintf(f, "  <settings pass=\"specialize\">\n"
               "    <component name=\"Microsoft-Windows-Deployment\" %s>\n"
               "      <RunSynchronous>\n"
               "        <RunSynchronousCommand wcm:action=\"add\">\n"
               "          <Order>1</Order>\n"
               "          <Path>reg add HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OOBE /v BypassNRO /t REG_DWORD /d 1 /f</Path>\n"
               "        </RunSynchronousCommand>\n"
               "      </RunSynchronous>\n    </component>\n  </settings>\n", comp);

  const char *cmds[32];
  char cmdbuf[512];
  int ncmd = first_logon(w, cmds, 32, cmdbuf, sizeof cmdbuf);
  int shell = w->privacy || w->user[0] || ncmd || (w->locale && w->tz[0]);
  if (shell || w->locale || w->bitlocker) {
    fprintf(f, "  <settings pass=\"oobeSystem\">\n");
    if (shell) {
      fprintf(f, "    <component name=\"Microsoft-Windows-Shell-Setup\" %s>\n", comp);
      if (w->privacy)
        fprintf(f, "      <OOBE>\n        <HideEULAPage>true</HideEULAPage>\n"
                   "        <ProtectYourPC>3</ProtectYourPC>\n"
                   "        <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>\n      </OOBE>\n");
      if (w->locale && w->tz[0]) fprintf(f, "      <TimeZone>%s</TimeZone>\n", w->tz);
      if (w->user[0]) {
        fprintf(f, "      <UserAccounts>\n        <LocalAccounts>\n"
                   "          <LocalAccount wcm:action=\"add\">\n            <Name>");
        xesc(f, w->user);
        fprintf(f, "</Name>\n            <DisplayName>");
        xesc(f, w->user);
        fprintf(f, "</DisplayName>\n            <Group>Administrators;Power Users</Group>\n"
                   "            <Password>\n              <Value>UABhAHMAcwB3AG8AcgBkAA==</Value>\n"
                   "              <PlainText>false</PlainText>\n            </Password>\n"
                   "          </LocalAccount>\n        </LocalAccounts>\n      </UserAccounts>\n");
      }
      if (ncmd) {
        fprintf(f, "      <FirstLogonCommands>\n");
        for (int i = 0; i < ncmd; i++) {
          fprintf(f, "        <SynchronousCommand wcm:action=\"add\">\n          <Order>%d</Order>\n"
                     "          <CommandLine>", i + 1);
          xesc(f, cmds[i]);
          fprintf(f, "</CommandLine>\n        </SynchronousCommand>\n");
        }
        fprintf(f, "      </FirstLogonCommands>\n");
      }
      fprintf(f, "    </component>\n");
    }
    if (w->locale)
      fprintf(f, "    <component name=\"Microsoft-Windows-International-Core\" %s>\n"
                 "      <InputLocale>%s</InputLocale>\n      <SystemLocale>%s</SystemLocale>\n"
                 "      <UserLocale>%s</UserLocale>\n      <UILanguage>%s</UILanguage>\n"
                 "      <UILanguageFallback>en-US</UILanguageFallback>\n    </component>\n",
              comp, w->kbd, w->lang, w->lang, w->lang);
    if (w->bitlocker)
      fprintf(f, "    <component name=\"Microsoft-Windows-SecureStartup-FilterDriver\" %s>\n"
                 "      <PreventDeviceEncryption>true</PreventDeviceEncryption>\n    </component>\n"
                 "    <component name=\"Microsoft-Windows-EnhancedStorage-Adm\" %s>\n"
                 "      <TCGSecurityActivationDisabled>1</TCGSecurityActivationDisabled>\n"
                 "    </component>\n", comp, comp);
    fprintf(f, "  </settings>\n");
  }
  fprintf(f, "</unattend>\n");
  if (fclose(f) != 0) {
    snprintf(err, cap, "cannot close '%s'", path);
    return -1;
  }
  return 0;
}

int rufux_write_unattend(const char *dir, const char *wue, char *err, unsigned long cap) {
  Wue w;
  int r = wue_parse(wue, &w, err, cap);
  if (r != 0) return r < 0 ? -1 : 0;
  if (w.locale) locale_fill(&w, NULL, NULL, NULL);
  char path[PATH_MAX];
  snprintf(path, sizeof path, "%s/autounattend.xml", dir);
  return write_xml(path, media_arch(dir), &w, w.bypass, err, cap);
}

static void say(RufuxWueLog log, void *lu, const char *m) {
  if (log) log(m, lu);
}

static int patch_boot_wim(const char *root, RufuxWueLog log, void *lu) {
  if (!rufux_have("wimlib-imagex") || !rufux_have("hivexsh")) {
    say(log, lu, "wimlib-imagex/hivex not installed: using the answer-file fallback for the Secure Boot/TPM/RAM bypass.");
    return 1;
  }
  char wim[PATH_MAX];
  if (resolve_ci_win(root, "sources/boot.wim", wim, sizeof wim) != 0) {
    say(log, lu, "boot.wim not found: using the answer-file fallback.");
    return 1;
  }
  char dir[] = "/tmp/rufux-wue-XXXXXX";
  if (!mkdtemp(dir)) return 1;

  // Ensure wimlib-imagex resolves /Windows/System32/{Config,config}/SYSTEM case-insensitively on Unix.
  setenv("WIMLIB_IMAGEX_IGNORE_CASE", "1", 1);

  char hive[PATH_MAX], dest[PATH_MAX + 32], cmd[PATH_MAX + 64], script[PATH_MAX], m[PATH_MAX + 80];
  snprintf(hive, sizeof hive, "%s/SYSTEM", dir);
  snprintf(dest, sizeof dest, "--dest-dir=%s", dir);
  int ok = 0;
  do {
    const char *ex[] = {"wimlib-imagex", "extract", wim, "2", "/Windows/System32/config/SYSTEM",
                        dest, "--no-acls", NULL};
    if (rufux_run(ex, 0) != 0) {
      say(log, lu, "Could not read the registry from boot.wim.");
      break;
    }
    snprintf(script, sizeof script, "%s/edit.hsh", dir);
    FILE *f = fopen(script, "w");
    if (!f) break;
    fputs("cd Setup\nadd LabConfig\ncd LabConfig\nsetval 3\n"
          "BypassTPMCheck\ndword:1\nBypassSecureBootCheck\ndword:1\nBypassRAMCheck\ndword:1\ncommit\n", f);
    fclose(f);
    const char *ed[] = {"hivexsh", "-w", "-f", script, hive, NULL};
    if (rufux_run(ed, 0) != 0) {
      say(log, lu, "Could not edit the registry hive.");
      break;
    }
    snprintf(cmd, sizeof cmd, "add %s /Windows/System32/config/SYSTEM", hive);
    snprintf(m, sizeof m, "--command=%s", cmd);
    const char *up[] = {"wimlib-imagex", "update", wim, "2", m, NULL};
    if (rufux_run(up, 0) != 0) {
      say(log, lu, "Could not write the registry back into boot.wim.");
      break;
    }
    char chk[PATH_MAX];
    snprintf(chk, sizeof chk, "%s/verify", dir);
    if (mkdir(chk, 0700) != 0) break;
    char dest2[PATH_MAX + 32];
    snprintf(dest2, sizeof dest2, "--dest-dir=%s", chk);
    const char *ex2[] = {"wimlib-imagex", "extract", wim, "2", "/Windows/System32/config/SYSTEM",
                         dest2, "--no-acls", NULL};
    if (rufux_run(ex2, 0) != 0) break;
    char hive2[PATH_MAX], vs[PATH_MAX], out[1024] = {0};
    snprintf(hive2, sizeof hive2, "%s/SYSTEM", chk);
    snprintf(vs, sizeof vs, "%s/verify.hsh", dir);
    FILE *vf = fopen(vs, "w");
    if (!vf) break;
    fputs("cd \\Setup\\LabConfig\nlsval\n", vf);
    fclose(vf);
    const char *g[] = {"hivexsh", "-f", vs, hive2, NULL};
    if (rufux_capture(g, out, sizeof out) != 0 ||
        !strstr(out, "\"BypassSecureBootCheck\"=dword:00000001")) {
      say(log, lu, "Registry change did not verify after writing boot.wim.");
      break;
    }
    ok = 1;
  } while (0);
  const char *rm[] = {"/bin/rm", "-rf", dir, NULL};
  (void)rufux_run(rm, 0);
  return ok ? 0 : 1;
}

int rufux_windows_customize(const char *root, const RufuxWueArgs *a, RufuxWueLog log, void *lu,
                            char *err, unsigned long cap) {
  const char *wue = a ? a->wue : NULL;
  Wue w;
  int pr = wue_parse(wue, &w, err, cap);
  if (pr < 0) return -1;
  if (w.locale) locale_fill(&w, a ? a->locale : NULL, a ? a->keyboard : NULL, a ? a->timezone : NULL);
  if (pr == 1) return 0;

  char m[PATH_MAX + 128], path[PATH_MAX];
  int fallback = 0;
  if (w.bypass) {
    if (patch_boot_wim(root, log, lu) == 0)
      say(log, lu, "Secure Boot/TPM/RAM checks disabled inside boot.wim (Setup screens unchanged).");
    else {
      fallback = 1;
      say(log, lu, "Using the answer-file fallback for the Secure Boot/TPM/RAM bypass. "
                   "It works, but it changes Setup's first screens and opens a command window.");
    }
  }

  if (w.user[0]) {
    snprintf(m, sizeof m, "Local account '%s' (empty password, changed at first logon).", w.user);
    say(log, lu, m);
  }
  if (w.locale) {
    snprintf(m, sizeof m, "Regional options: language %s, keyboard %s, time zone %s.", w.lang, w.kbd,
             w.tz[0] ? w.tz : "(not recognised, left to Windows)");
    say(log, lu, m);
  }
  if (fallback || w.nro || w.privacy || w.bitlocker || w.locale || w.qol || w.user[0]) {
    const char *arch = media_arch(root);
    if (fallback) {
      snprintf(path, sizeof path, "%s/autounattend.xml", root);
    } else {
      char d1[PATH_MAX], d2[PATH_MAX], d3[PATH_MAX], d4[PATH_MAX];
      if (ensure_child_dir_ci(root, "sources", d1, sizeof d1) != 0) { snprintf(path, sizeof path, "%s/sources", root); goto mkfail; }
      if (ensure_child_dir_ci(d1, "$OEM$", d2, sizeof d2) != 0) { snprintf(path, sizeof path, "%s/$OEM$", d1); goto mkfail; }
      if (ensure_child_dir_ci(d2, "$$", d3, sizeof d3) != 0) { snprintf(path, sizeof path, "%s/$$", d2); goto mkfail; }
      if (ensure_child_dir_ci(d3, "Panther", d4, sizeof d4) != 0) { snprintf(path, sizeof path, "%s/Panther", d3); goto mkfail; }
      snprintf(path, sizeof path, "%s/unattend.xml", d4);
    }
    if (write_xml(path, arch, &w, fallback, err, cap) != 0) return -1;
    snprintf(m, sizeof m, "Answer file written to %s", path + strlen(root) + 1);
    say(log, lu, m);
  }
  return 0;
mkfail:
  snprintf(err, cap, "cannot create '%s': %s", path, strerror(errno));
  return -1;
}

int rufux_windows_check(const RufuxWueArgs *a, char *err, unsigned long cap) {
  Wue w;
  if (wue_parse(a ? a->wue : NULL, &w, err, cap) < 0) return -1;
  return 0;
}

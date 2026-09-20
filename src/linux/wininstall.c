#define _GNU_SOURCE
#include "wininstall.h"
#include "iso_probe.h"
#include "exec.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <strings.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <errno.h>
#include <dirent.h>

int rufux_is_windows_iso(const char *iso, char *err, unsigned long cap) {
  struct stat st;
  if (stat(iso, &st) != 0) { snprintf(err, cap, "source '%s' missing", iso); return -1; }
  // UDF images defeat bsdtar's listing the same way they defeat its
  // extraction: list those with 7z instead.
  if (rufux_iso_is_udf(iso) > 0) {
    if (!rufux_have("7z")) { snprintf(err, cap, "need 7z to inspect UDF image"); return -1; }
    const char *av[] = {"7z", "l", "-ba", iso, NULL};
    char out[65536] = {0};
    if (rufux_capture(av, out, sizeof out) != 0) return 0;
    char *save = NULL, *line = strtok_r(out, "\n", &save);
    while (line) {
      while (*line == ' ' || *line == '\t') line++;
      // trim trailing whitespace/CR (7z emits \r\n)
      char *e = line + strlen(line);
      while (e > line && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
      const char *t = line + strlen(line);
      while (t > line && t[-1] != '/' && t[-1] != '\\') t--;
      if (!strcasecmp(t, "install.wim") || !strcasecmp(t, "install.esd") ||
          !strcasecmp(t, "install.swm"))
        return 1;
      line = strtok_r(NULL, "\n", &save);
    }
    return 0;
  }
  if (!rufux_have("bsdtar")) { snprintf(err, cap, "need bsdtar to inspect ISO"); return -1; }
  const char *av[] = {"bsdtar", "-tf", iso, NULL};
  char out[65536] = {0};
  if (rufux_capture(av, out, sizeof out) != 0) return 0; // unreadable: not windows
  char *save = NULL, *line = strtok_r(out, "\n", &save);
  while (line) {
    // sources/install.wim (or .esd/.swm) marks Windows install media
    if (strcasestr(line, "sources/install.wim") || strcasestr(line, "sources/install.esd") ||
        strcasestr(line, "sources/install.swm")) {
      // bsdtar prefixes ./ sometimes; match the tail explicitly
      const char *t = line + strlen(line);
      while (t > line && t[-1] != '/') t--;
      if (!strcasecmp(t, "install.wim") || !strcasecmp(t, "install.esd") ||
          !strcasecmp(t, "install.swm"))
        return 1;
    }
    line = strtok_r(NULL, "\n", &save);
  }
  return 0;
}

// UEFI:NTFS ESP payload: the full EFI tree (EFI/Boot/* loaders plus
// EFI/Rufus/ntfs_*.efi and exfat_*.efi drivers) from res/uefi/uefi-ntfs.img.
// The loader alone is not enough: it looks for \EFI\Rufus\ntfs_<arch>.efi
// beside itself and aborts with "couldn't find/load NTFS driver" when the
// driver file is missing, so the whole tree must be staged, like Rufus.
// The image ships in-tree (and installed under share/rufux); the upstream
// download is only a fallback when no local copy resolves.
#define UEFI_NTFS_IMG_URL "https://raw.githubusercontent.com/pbatard/rufus/master/res/uefi/uefi-ntfs.img"
#define UEFI_NTFS_IMG_SIZE 1048576UL

static int cache_dir(char *out, unsigned long cap) {
  const char *base = getenv("XDG_CACHE_HOME");
  if (base && base[0]) snprintf(out, cap, "%s/rufux", base);
  else {
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = "/tmp";
    snprintf(out, cap, "%s/.cache/rufux", home);
  }
  char cmd[1152];
  snprintf(cmd, sizeof cmd, "%s", out);
  // mkdir -p equivalent
  char tmp[1152];
  snprintf(tmp, sizeof tmp, "%s", out);
  for (char *c = tmp + 1; *c; c++) {
    if (*c == '/') {
      *c = 0;
      mkdir(tmp, 0755);
      *c = '/';
    }
  }
  struct stat st;
  if (mkdir(out, 0755) != 0 && errno != EEXIST) return -1;
  if (stat(out, &st) != 0) return -1;
  return 0;
}

// mkdir -p helper (shared by the staging below).
static int mkdir_p(const char *path) {
  char tmp[1152];
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

// Resolve the local UEFI:NTFS image, mirroring the FreeDOS payload lookup
// (RUFUX_RES override, build tree, installed shares, exe-relative).
static const char *uefi_img_local(void) {
  static char path[1152];
  const char *env = getenv("RUFUX_RES");
  if (env && env[0]) {
    snprintf(path, sizeof path, "%s/uefi/uefi-ntfs.img", env);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 100000) return path;
  }
  static char exedir[1024] = {0};
  if (!exedir[0]) {
    ssize_t n = readlink("/proc/self/exe", exedir, sizeof exedir - 1);
    if (n > 0) {
      exedir[n] = 0;
      char *slash = strrchr(exedir, '/');
      if (slash) *slash = 0;
    }
  }
  static const char *cands[] = {
    "res/uefi/uefi-ntfs.img", // build tree
    "/usr/share/rufux/uefi-ntfs.img",
    "/usr/local/share/rufux/uefi-ntfs.img",
    NULL,
  };
  char probe[1152];
  struct stat st;
  if (exedir[0]) {
    snprintf(probe, sizeof probe, "%s/../share/rufux/uefi-ntfs.img", exedir);
    if (stat(probe, &st) == 0 && st.st_size > 100000) {
      snprintf(path, sizeof path, "%s", probe);
      return path;
    }
  }
  for (int i = 0; cands[i]; i++) {
    if (stat(cands[i], &st) == 0 && st.st_size > 100000) {
      snprintf(path, sizeof path, "%s", cands[i]);
      return path;
    }
  }
  return NULL;
}

// Path of the UEFI:NTFS image: local copy first, upstream download cached
// as a fallback when nothing resolves (offline then fails loudly).
static int uefi_img_path(char *out, unsigned long cap, char *err, unsigned long errcap) {
  const char *local = uefi_img_local();
  if (local) { snprintf(out, cap, "%s", local); return 0; }
  char dir[1024];
  if (cache_dir(dir, sizeof dir) != 0) {
    snprintf(err, errcap, "cannot create cache dir");
    return -1;
  }
  snprintf(out, cap, "%s/uefi-ntfs.img", dir);
  struct stat st;
  if (stat(out, &st) == 0 && (unsigned long)st.st_size >= UEFI_NTFS_IMG_SIZE) return 0; // cached
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

int rufux_stage_uefi_ntfs(const char *tmpdir, char *err, unsigned long cap) {
  char img[1152];
  if (uefi_img_path(img, sizeof img, err, cap) != 0) return -1;
  if (!rufux_have("7z")) {
    snprintf(err, cap, "need 7z to unpack the UEFI:NTFS image");
    return -1;
  }
  char esp[1152], bootd[1152], rufusd[1152], xtr[1152];
  snprintf(esp, sizeof esp, "%s/esp", tmpdir);
  snprintf(bootd, sizeof bootd, "%s/esp/EFI/BOOT", tmpdir);
  snprintf(rufusd, sizeof rufusd, "%s/esp/EFI/Rufus", tmpdir);
  snprintf(xtr, sizeof xtr, "%s/.uefintfs", tmpdir);
  if (mkdir_p(bootd) != 0 || mkdir_p(rufusd) != 0) {
    snprintf(err, cap, "cannot mkdir ESP staging dirs");
    return -1;
  }
  if (mkdir_p(xtr) != 0) { snprintf(err, cap, "cannot mkdir extract dir"); return -1; }
  // The image is a raw FAT filesystem; 7z reads it, bsdtar does not.
  char out[1250];
  snprintf(out, sizeof out, "-o%s", xtr);
  const char *av[] = {"7z", "x", "-y", img, out, NULL};
  if (rufux_run(av, 0) != 0) {
    snprintf(err, cap, "cannot extract UEFI:NTFS image");
    return -1;
  }
  // Copy the EFI tree, normalizing the loader dir to the BOOT casing the
  // rest of the code validates (FAT itself is case-insensitive).
  char s1[1250], t1[1250], s2[1250], t2[1250];
  snprintf(s1, sizeof s1, "%s/EFI/Boot/.", xtr);
  snprintf(t1, sizeof t1, "%s", bootd);
  snprintf(s2, sizeof s2, "%s/EFI/Rufus/.", xtr);
  snprintf(t2, sizeof t2, "%s", rufusd);
  const char *cp1[] = {"cp", "-a", s1, t1, NULL};
  const char *cp2[] = {"cp", "-a", s2, t2, NULL};
  if (rufux_run(cp1, 0) != 0 || rufux_run(cp2, 0) != 0) {
    snprintf(err, cap, "cannot stage UEFI:NTFS tree");
    return -1;
  }
  char rm[1250];
  snprintf(rm, sizeof rm, "%s", xtr);
  const char *rmv[] = {"rm", "-rf", rm, NULL};
  rufux_run(rmv, 0); // best effort cleanup
  // Both halves must have landed: the loader the firmware runs and the
  // NTFS driver it refuses to boot without.
  char boot[1152], drv[1152];
  snprintf(boot, sizeof boot, "%s/esp/EFI/BOOT/bootx64.efi", tmpdir);
  snprintf(drv, sizeof drv, "%s/esp/EFI/Rufus/ntfs_x64.efi", tmpdir);
  struct stat bst, dst;
  if (stat(boot, &bst) != 0 || bst.st_size < 10000) {
    snprintf(err, cap, "staged bootloader missing or too small");
    return -1;
  }
  if (stat(drv, &dst) != 0 || dst.st_size < 10000) {
    snprintf(err, cap, "staged NTFS driver missing (EFI/Rufus/ntfs_x64.efi)");
    return -1;
  }
  return 0;
}

// Write the UEFI:NTFS image raw onto its own (1 MiB) partition, exactly like
// Rufus: the image is a complete FAT filesystem holding the EFI loaders and
// the NTFS/exFAT drivers, so no mkfs, mount or file copy is involved. The
// result is read back and compared before we declare success.
int rufux_write_uefi_ntfs(const char *part_dev, char *err, unsigned long cap) {
  char img[1152];
  if (uefi_img_path(img, sizeof img, err, cap) != 0) return -1;
  FILE *in = fopen(img, "rb");
  if (!in) { snprintf(err, cap, "cannot open '%s': %s", img, strerror(errno)); return -1; }
  static unsigned char want[UEFI_NTFS_IMG_SIZE], got[UEFI_NTFS_IMG_SIZE];
  size_t n = fread(want, 1, sizeof want, in);
  fclose(in);
  if (n != sizeof want) {
    snprintf(err, cap, "UEFI:NTFS image '%s' is %zu bytes, expected %lu", img, n, UEFI_NTFS_IMG_SIZE);
    return -1;
  }
  int fd = open(part_dev, O_RDWR | O_EXCL | O_CLOEXEC);
  if (fd < 0) { snprintf(err, cap, "cannot open '%s': %s", part_dev, strerror(errno)); return -1; }
  unsigned long long psz = 0;
  if (ioctl(fd, BLKGETSIZE64, &psz) != 0 || psz < UEFI_NTFS_IMG_SIZE) {
    snprintf(err, cap, "partition '%s' is smaller than the UEFI:NTFS image", part_dev);
    close(fd);
    return -1;
  }
  size_t off = 0;
  while (off < sizeof want) {
    ssize_t w = pwrite(fd, want + off, sizeof want - off, (off_t)off);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) { snprintf(err, cap, "writing UEFI:NTFS image failed: %s", strerror(errno)); close(fd); return -1; }
    off += (size_t)w;
  }
  if (fsync(fd) != 0) { snprintf(err, cap, "fsync of '%s' failed", part_dev); close(fd); return -1; }
  // Drop cached pages so the read-back hits the device, not the page cache.
  ioctl(fd, BLKFLSBUF, 0);
  off = 0;
  while (off < sizeof got) {
    ssize_t r = pread(fd, got + off, sizeof got - off, (off_t)off);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    off += (size_t)r;
  }
  close(fd);
  if (off != sizeof got || memcmp(want, got, sizeof want) != 0) {
    snprintf(err, cap, "UEFI:NTFS image read-back mismatch on '%s'", part_dev);
    return -1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Windows User Experience, done the way Rufus does it.
//
// Bypassing the Secure Boot / TPM / RAM checks is done by writing the
// LabConfig keys into the SYSTEM registry hive inside sources\boot.wim
// (index 2, Windows Setup). That leaves Setup's screens untouched. An answer
// file with a windowsPE pass is the fallback only, because it changes the
// installer's flow (Rufus documents this in wue.c). Answer files without a
// windowsPE pass go to sources\$OEM$\$$\Panther\unattend.xml like Rufus
// does; ones with it go to the media root as autounattend.xml.
// ---------------------------------------------------------------------------
typedef struct {
  int bypass, nro, privacy, bitlocker, locale, qol;
  char user[64];  // local account name, "" = none
  char lang[32];  // BCP-47 tag, e.g. en-US
  char kbd[32];   // Windows keyboard id, e.g. 0409:00000409
  char tz[64];    // Windows time zone name, "" = unknown
} Wue;

// Names Windows refuses for a local account (Rufus keeps the same idea).
static const char *const bad_names[] = {
  "administrator", "guest", "defaultaccount", "wdagutilityaccount", "system",
  "network", "local service", "network service", "krbtgt", "root", NULL};

// Returns 0 ok, 1 for "none" (nothing to do), -1 on a bad item.
// Items: bypass,nro,privacy,bitlocker,locale,qol,all and user=NAME.
static int wue_parse(const char *spec, Wue *w, char *err, unsigned long cap) {
  memset(w, 0, sizeof *w);
  if (spec && !strcmp(spec, "none")) return 1;
  if (!spec || !spec[0]) { w->bypass = 1; return 0; }  // default: requirement bypass only
  char tmp[512];
  snprintf(tmp, sizeof tmp, "%s", spec);
  for (char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ",")) {
    while (*tok == ' ') tok++;
    if (!strncasecmp(tok, "user=", 5)) {
      snprintf(w->user, sizeof w->user, "%s", tok + 5);
      // Same replacement Rufus applies to characters Windows rejects.
      for (char *c = w->user; *c; c++)
        if (strchr("\"/\\[]:;|=,+*?<>@&", *c) || (unsigned char)*c < 32) *c = '_';
      for (int i = 0; bad_names[i]; i++)
        if (!strcasecmp(w->user, bad_names[i])) {
          snprintf(err, cap, "'%s' is not allowed as a local account name", w->user);
          return -1;
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

// Setup needs processorArchitecture to match the media.
static const char *media_arch(const char *root) {
  char p[1152];
  snprintf(p, sizeof p, "%s/efi/boot/bootaa64.efi", root);
  if (access(p, F_OK) == 0) return "arm64";
  snprintf(p, sizeof p, "%s/efi/boot/bootx64.efi", root);
  if (access(p, F_OK) == 0) return "amd64";
  snprintf(p, sizeof p, "%s/efi/boot/bootia32.efi", root);
  if (access(p, F_OK) == 0) return "x86";
  return "amd64";
}

// ---- regional options: Linux -> Windows names ------------------------------
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
  {"Europe/Helsinki", "FLE Standard Time"}, {"Europe/Kiev", "FLE Standard Time"},
  {"Europe/Kyiv", "FLE Standard Time"}, {"Europe/Istanbul", "Turkey Standard Time"},
  {"Europe/Moscow", "Russian Standard Time"}, {"Asia/Riyadh", "Arab Standard Time"},
  {"Asia/Dubai", "Arabian Standard Time"}, {"Asia/Baghdad", "Arabic Standard Time"},
  {"Asia/Jerusalem", "Israel Standard Time"}, {"Asia/Tehran", "Iran Standard Time"},
  {"Asia/Kolkata", "India Standard Time"}, {"Asia/Karachi", "Pakistan Standard Time"},
  {"Asia/Dhaka", "Bangladesh Standard Time"}, {"Asia/Bangkok", "SE Asia Standard Time"},
  {"Asia/Shanghai", "China Standard Time"}, {"Asia/Hong_Kong", "China Standard Time"},
  {"Asia/Singapore", "Singapore Standard Time"}, {"Asia/Tokyo", "Tokyo Standard Time"},
  {"Asia/Seoul", "Korea Standard Time"}, {"Australia/Sydney", "AUS Eastern Standard Time"},
  {"Pacific/Auckland", "New Zealand Standard Time"}, {"America/New_York", "Eastern Standard Time"},
  {"America/Toronto", "Eastern Standard Time"}, {"America/Chicago", "Central Standard Time"},
  {"America/Denver", "Mountain Standard Time"}, {"America/Los_Angeles", "Pacific Standard Time"},
  {"America/Sao_Paulo", "E. South America Standard Time"},
  {"America/Argentina/Buenos_Aires", "Argentina Standard Time"},
  {"America/Mexico_City", "Central Standard Time (Mexico)"}, {NULL, NULL}};

// Keyboard layout ids (KLID) per primary language, the usual layout for it.
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
  {NULL, NULL}};

// "en_US.UTF-8" -> "en-US"
static void lang_tag(const char *in, char *out, size_t cap) {
  snprintf(out, cap, "%s", in && in[0] && strcmp(in, "C") && strcmp(in, "POSIX") ? in : "en-US");
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
  if (strstr(zone, "Standard Time") || !strcmp(zone, "UTC")) { snprintf(out, cap, "%s", zone); return; }
  for (int i = 0; tz_map[i].iana; i++)
    if (!strcmp(tz_map[i].iana, zone)) { snprintf(out, cap, "%s", tz_map[i].win); return; }
}

// Fill language/keyboard/time zone from what the caller passed, else from this machine.
static void locale_fill(Wue *w, const char *lang, const char *kbd, const char *tz) {
  lang_tag(lang && lang[0] ? lang : (getenv("LC_ALL") ? getenv("LC_ALL") : getenv("LANG")),
           w->lang, sizeof w->lang);
  snprintf(w->kbd, sizeof w->kbd, "%s", kbd && kbd[0] ? kbd : klid_for(w->lang));
  char zone[128] = {0};
  if (tz && tz[0]) snprintf(zone, sizeof zone, "%s", tz);
  else {
    char l[256];
    ssize_t n = readlink("/etc/localtime", l, sizeof l - 1);
    if (n > 0) {
      l[n] = 0;
      const char *z = strstr(l, "zoneinfo/");
      if (z) snprintf(zone, sizeof zone, "%s", z + 9);
    }
  }
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

// Commands that run once at first logon (Windows allows a single such list).
static int first_logon(const Wue *w, const char *out[], size_t max, char *bufs, size_t bufcap) {
  int n = 0;
  size_t used = 0;
  if (w->user[0]) {
    // The account has an empty password, so ask for a new one at next logon.
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
      NULL};
    for (int i = 0; q[i] && (size_t)n < max; i++) out[n++] = q[i];
  }
  return n;
}

// Write an answer file. The windowsPE pass exists only when `winpe_bypass`
// asks for the registry commands (the fallback path).
static int write_xml(const char *path, const char *arch, const Wue *w, int winpe_bypass,
                     char *err, unsigned long cap) {
  FILE *f = fopen(path, "w");
  if (!f) { snprintf(err, cap, "cannot write '%s': %s", path, strerror(errno)); return -1; }
  char comp[512];
  snprintf(comp, sizeof comp, "processorArchitecture=\"%s\" publicKeyToken=\"31bf3856ad364e35\" "
           "language=\"neutral\" versionScope=\"nonSxS\" "
           "xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" "
           "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"", arch);
  fprintf(f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
             "<!-- Generated by Rufux (Windows User Experience). -->\n"
             "<unattend xmlns=\"urn:schemas-microsoft-com:unattend\">\n");
  if (winpe_bypass) {
    static const char *keys[] = {"BypassTPMCheck", "BypassSecureBootCheck", "BypassRAMCheck",
                                 "BypassCPUCheck", "BypassStorageCheck", NULL};
    fprintf(f, "  <settings pass=\"windowsPE\">\n"
               "    <component name=\"Microsoft-Windows-Setup\" %s>\n"
               // WinPE complains without a product key; any (empty) key will do.
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
                   // Despite its name this only controls data collection.
                   "        <ProtectYourPC>3</ProtectYourPC>\n"
                   "        <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>\n      </OOBE>\n");
      if (w->locale && w->tz[0]) fprintf(f, "      <TimeZone>%s</TimeZone>\n", w->tz);
      if (w->user[0]) {
        // Creating a local account also lets Windows 11 22H2+ skip the Microsoft
        // account page even with a network connection. Empty password: the value
        // is the base64 UTF-16 string "Password" with PlainText=false (Rufus does
        // the same), and first-logon commands below ask for a new one.
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
  if (fclose(f) != 0) { snprintf(err, cap, "cannot close '%s'", path); return -1; }
  return 0;
}

// Compatibility entry point (used by the unit test and older callers):
// writes autounattend.xml in `dir`, with the registry commands in a
// windowsPE pass when `bypass` is requested.
int rufux_write_unattend(const char *dir, const char *wue, char *err, unsigned long cap) {
  Wue w;
  int r = wue_parse(wue, &w, err, cap);
  if (r != 0) return r < 0 ? -1 : 0;
  if (w.locale) locale_fill(&w, NULL, NULL, NULL);
  char path[1152];
  snprintf(path, sizeof path, "%s/autounattend.xml", dir);
  return write_xml(path, media_arch(dir), &w, w.bypass, err, cap);
}

static void say(RufuxWueLog log, void *lu, const char *m) { if (log) log(m, lu); }

// Write BypassTPMCheck / BypassSecureBootCheck / BypassRAMCheck into the
// SYSTEM hive of boot.wim index 2. Returns 0 on success (and verified), 1 if
// the tools are missing or anything failed (the caller then falls back to the
// answer-file route). The stick is never left half-modified: wimlib replaces
// the archive atomically and the result is read back.
static int patch_boot_wim(const char *root, RufuxWueLog log, void *lu) {
  if (!rufux_have("wimlib-imagex") || !rufux_have("hivexsh") || !rufux_have("hivexget")) {
    say(log, lu, "wimlib-imagex/hivex not installed: using the answer-file fallback for the Secure Boot/TPM/RAM bypass.");
    return 1;
  }
  char wim[1152];
  snprintf(wim, sizeof wim, "%s/sources/boot.wim", root);
  struct stat st;
  if (stat(wim, &st) != 0) { say(log, lu, "boot.wim not found: using the answer-file fallback."); return 1; }
  char dir[] = "/tmp/rufux-wue-XXXXXX";
  if (!mkdtemp(dir)) return 1;
  char hive[1200], dest[1200], cmd[1500], script[1200], m[600];
  snprintf(hive, sizeof hive, "%s/SYSTEM", dir);
  snprintf(dest, sizeof dest, "--dest-dir=%s", dir);
  int ok = 0;
  do {
    const char *ex[] = {"wimlib-imagex", "extract", wim, "2", "/Windows/System32/config/SYSTEM",
                        dest, "--no-acls", NULL};
    if (rufux_run(ex, 0) != 0) { say(log, lu, "Could not read the registry from boot.wim."); break; }
    snprintf(script, sizeof script, "%s/edit.hsh", dir);
    FILE *f = fopen(script, "w");
    if (!f) break;
    fputs("cd Setup\nadd LabConfig\ncd LabConfig\nsetval 3\n"
          "BypassTPMCheck\ndword:1\nBypassSecureBootCheck\ndword:1\nBypassRAMCheck\ndword:1\ncommit\n", f);
    fclose(f);
    const char *ed[] = {"hivexsh", "-w", "-f", script, hive, NULL};
    if (rufux_run(ed, 0) != 0) { say(log, lu, "Could not edit the registry hive."); break; }
    snprintf(cmd, sizeof cmd, "add %s /Windows/System32/config/SYSTEM", hive);
    snprintf(m, sizeof m, "--command=%s", cmd);
    const char *up[] = {"wimlib-imagex", "update", wim, "2", m, NULL};
    if (rufux_run(up, 0) != 0) { say(log, lu, "Could not write the registry back into boot.wim."); break; }
    // Read it back from the archive we just wrote.
    char chk[1200];
    snprintf(chk, sizeof chk, "%s/verify", dir);
    if (mkdir(chk, 0700) != 0) break;
    char dest2[1300];
    snprintf(dest2, sizeof dest2, "--dest-dir=%s", chk);
    const char *ex2[] = {"wimlib-imagex", "extract", wim, "2", "/Windows/System32/config/SYSTEM",
                         dest2, "--no-acls", NULL};
    if (rufux_run(ex2, 0) != 0) break;
    char hive2[1300], out[64] = {0};
    snprintf(hive2, sizeof hive2, "%s/SYSTEM", chk);
    const char *g[] = {"hivexget", hive2, "\\Setup\\LabConfig", "BypassSecureBootCheck", NULL};
    if (rufux_capture(g, out, sizeof out) != 0 || atoi(out) != 1) {
      say(log, lu, "Registry change did not verify after writing boot.wim.");
      break;
    }
    ok = 1;
  } while (0);
  const char *rm[] = {"rm", "-rf", "--", dir, NULL};
  rufux_run(rm, 0);
  return ok ? 0 : 1;
}

static int has_inf(const char *dir, int depth) {
  DIR *d = opendir(dir);
  if (!d) return 0;
  struct dirent *e;
  int found = 0;
  while (!found && (e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    size_t n = strlen(e->d_name);
    if (n > 4 && !strcasecmp(e->d_name + n - 4, ".inf")) { found = 1; break; }
    if (depth > 0) {
      char sub[1400];
      snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
      struct stat st;
      if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) found = has_inf(sub, depth - 1);
    }
  }
  closedir(d);
  return found;
}

int rufux_windows_customize(const char *root, const RufuxWueArgs *a, RufuxWueLog log, void *lu,
                            char *err, unsigned long cap) {
  const char *wue = a->wue, *drivers = a->drivers;
  Wue w;
  int pr = wue_parse(wue, &w, err, cap);
  if (pr < 0) return -1;
  if (w.locale) locale_fill(&w, a->locale, a->keyboard, a->timezone);
  char m[1400], path[1300];

  // 1. Drivers: Windows Setup loads everything under $WinPEDriver$ at the
  //    root of the install media, which is how missing storage controllers
  //    (Intel RST/VMD, some NVMe/RAID) become visible.
  if (drivers && drivers[0]) {
    struct stat st;
    if (stat(drivers, &st) != 0 || !S_ISDIR(st.st_mode)) {
      snprintf(err, cap, "drivers folder '%s' does not exist", drivers);
      return -1;
    }
    if (!has_inf(drivers, 4)) {
      snprintf(err, cap, "no .inf driver files found under '%s'", drivers);
      return -1;
    }
    snprintf(path, sizeof path, "%s/$WinPEDriver$", root);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
      snprintf(err, cap, "cannot create '%s': %s", path, strerror(errno));
      return -1;
    }
    char src[1300];
    snprintf(src, sizeof src, "%s/.", drivers);
    const char *cp[] = {"cp", "-a", "--", src, path, NULL};
    if (rufux_run(cp, 0) != 0) { snprintf(err, cap, "copying drivers failed"); return -1; }
    snprintf(m, sizeof m, "Drivers copied to $WinPEDriver$ from %s", drivers);
    say(log, lu, m);
  }
  if (pr == 1) return 0; // wue "none"

  // 2. Hardware-requirement bypass under the hood; answer file if that fails.
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

  // 3. Everything else goes into an answer file.
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
      snprintf(path, sizeof path, "%s/sources/$OEM$", root);
      if (mkdir(path, 0755) != 0 && errno != EEXIST) goto mkfail;
      snprintf(path, sizeof path, "%s/sources/$OEM$/$$", root);
      if (mkdir(path, 0755) != 0 && errno != EEXIST) goto mkfail;
      snprintf(path, sizeof path, "%s/sources/$OEM$/$$/Panther", root);
      if (mkdir(path, 0755) != 0 && errno != EEXIST) goto mkfail;
      snprintf(path, sizeof path, "%s/sources/$OEM$/$$/Panther/unattend.xml", root);
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

// Validate the customization options before the disk is touched, so a typo in
// an account name or a wrong drivers folder fails in a second, not after a
// ten-minute copy.
int rufux_windows_check(const RufuxWueArgs *a, char *err, unsigned long cap) {
  Wue w;
  if (wue_parse(a->wue, &w, err, cap) < 0) return -1;
  if (a->drivers && a->drivers[0]) {
    struct stat st;
    if (stat(a->drivers, &st) != 0 || !S_ISDIR(st.st_mode)) {
      snprintf(err, cap, "drivers folder '%s' does not exist", a->drivers);
      return -1;
    }
    if (!has_inf(a->drivers, 4)) {
      snprintf(err, cap, "no .inf driver files found under '%s'", a->drivers);
      return -1;
    }
  }
  return 0;
}

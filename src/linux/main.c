// Rufux entry point: argument parsing and command dispatch. No windows.h.
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include "linux/device.h"
#include "linux/iso_probe.h"
#include "linux/checksum.h"
#include "linux/writer.h"
#include "linux/partition.h"
#include "linux/mkfs_task.h"
#include "linux/extract.h"
#include "linux/boot.h"
#include "linux/persist.h"
#include "linux/badblocks.h"
#include "linux/mount.h"
#include "linux/priv.h"
#include "linux/secureboot.h"
#include "linux/update.h"
#include "linux/smart.h"
#include "linux/bootcheck.h"
#include "linux/msdl.h"
#include "linux/vhd.h"
#include "linux/vdisk.h"
#include "linux/exec.h"
#include "linux/i18n.h"
#include "linux/create.h"
#include "gui/gui.h"

#ifndef RUFUX_VERSION
#define RUFUX_VERSION "1.0.0"
#endif

static void usage(const char *p) {
  printf("Rufux %s (Stable)\n"
         "Usage:\n"
         "  %s list [--json] [--allow-fixed]\n"
         "  %s probe <file.iso> [--detail]\n"
         "  %s checksum <file> [--algo md5|sha1|sha256|sha512]\n"
         "  %s write SRC DST [--dry-run|--real] [--verify] [--allow-fixed] [--allow-file] [--yes]\n"
         "  %s partition DST --scheme gpt|dos --layout single|esp+main [--fs vfat|fat16|ntfs|exfat|ext2|ext3|ext4] [--dry-run|--real] [--allow-file] [--allow-fixed] [--yes]\n"
         "  %s format DST --fs vfat|fat16|ntfs|exfat|ext2|ext3|ext4|udf [--label L] [--dry-run|--real] [--allow-file] [--allow-fixed] [--yes]\n"
         "  %s extract SRC.iso DEST_DIR [--dry-run]\n"
         "  %s install-boot DST --mbr bios|gpt [--dry-run|--real] [--allow-file] [--allow-fixed] [--yes]\n"
         "  %s persist DIR --size MB [--label casper-rw] [--dry-run]\n"
         "  %s badblocks DST [--allow-file] [--write-patterns 1..4 --yes]\n"
         "  %s mount|umount DEV [--dry-run]\n"
         "  %s secureboot-status\n"
         "  %s validate-efi FILE\n"
         "  %s smart DEV\n"
         "  %s check-boot FILE.efi|DIR\n"
         "  %s update-check\n"
         "  %s create SRC|none DST --mode dd|extract|format|dos|windows [--scheme gpt|dos] [--fs vfat|fat16|ntfs|exfat|ext2|ext3|ext4|udf] [--label L] [--persist-mb N] [--cluster-sectors N] [--badblock-passes N] [--wue bypass,nro,privacy,bitlocker,locale,qol,user=NAME,all,none] [--split-wim MB] [--locale TAG] [--keyboard KLID] [--timezone ZONE] [--quick|--full] [--no-autorun] [--uefi-validate] [--ignore-smart] [--dry-run|--real] [--allow-file] [--allow-fixed] [--yes] [--verify]\n"
         "  %s download-windows [--list | --list-langs] [--version 11|10] [--edition N] [--lang NAME] [--arch x64|ARM64|x86] [--out DIR] [--url-only]\n"
         "  %s --gui [--theme system|dark|light]\n",
         RUFUX_VERSION, p, p, p, p, p, p, p, p, p, p, p, p, p, p, p, p, p, p, p);
}

// Download progress: a redrawn line on a terminal, one "N%" line per percent otherwise
// (the GUI's progress bar reads those).
static void dl_progress(unsigned long long done, unsigned long long total, void *u) {
  (void)u;
  static int last = -1;
  int pct = total ? (int)(done * 100 / total) : 0;
  if (pct == last) return;
  last = pct;
  if (isatty(fileno(stderr))) fprintf(stderr, "\rDownloading... %d%%%s", pct, pct >= 100 ? "\n" : "");
  else fprintf(stderr, "%d%%\n", pct);
  fflush(stderr);
}

static void cli_progress(unsigned long long done, unsigned long long total, void *u) {
  (void)u;
  static int last = -1;
  static double t0 = 0;
  // A real terminal redraws in place with '\r'. Piped output (the GUI's
  // pkexec worker included) is read a line at a time, so a '\r'-only
  // update with no '\n' never completes a line and the reader never sees
  // it until the process happens to print one - which used to mean the
  // GUI's progress bar sat at 0 until the final 100% line arrived. Use
  // '\n' whenever stderr isn't a terminal so every update is its own line.
  static int tty = -1;
  if (tty < 0) tty = isatty(fileno(stderr));
  const char *eol = tty ? "" : "\n";
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  double now = ts.tv_sec + ts.tv_nsec / 1e9;
  // Percent-normalized mode (overall flow progress): total == 100.
  if (total == 100 && done <= 100) {
    int pct = (int)done;
    if (pct != last) {
      fprintf(stderr, "\r%3d%%%s", pct, eol);
      if (done == total && tty) fprintf(stderr, "\n");
      last = pct;
    }
    return;
  }
  if (t0 == 0 || done == 0) { t0 = now; last = -1; }
  int pct = total ? (int)(done * 100 / total) : 100;
  if (pct != last && (pct % 5 == 0 || done == total)) {
    double el = now - t0 > 0 ? now - t0 : 0.001;
    double rate = done / el / 1048576.0; // MB/s
    if (done == total || rate <= 0) {
      fprintf(stderr, "\r%3d%%  %llu/%llu MB  %.1f MB/s%s", pct,
              done >> 20, total >> 20, rate, eol);
    } else {
      unsigned eta = (unsigned)((total - done) / done * el);
      fprintf(stderr, "\r%3d%%  %llu/%llu MB  %.1f MB/s  ETA %u:%02u%s", pct,
              done >> 20, total >> 20, rate, eta / 60, eta % 60, eol);
    }
    if (done == total && tty) fprintf(stderr, "\n");
    last = pct;
  }
}

static void cli_clog(const char *m, void *u) {
  (void)u;
  printf("%s\n", m);
}

// End-to-end flows live in src/linux/create.c (shared with the GUI).

// ---------------------------------------------------------------------------
// Strict option checking. A typo such as "--rela" must never be read as "no
// option": for a tool that writes disks, "I thought I passed --dry-run" is the
// worst mistake to make silently. Every option of every command is listed here.
// ---------------------------------------------------------------------------
typedef struct { const char *cmd, *flags, *values; } CmdSpec;
static const CmdSpec specs[] = {
  {"list", "--allow-fixed --json", ""},
  {"probe", "--detail", ""},
  {"checksum", "", "--algo"},
  {"write", "--allow-file --allow-fixed --dry-run --real --verify --yes", ""},
  {"partition", "--allow-file --allow-fixed --dry-run --real --yes", "--fs --layout --scheme"},
  {"format", "--allow-file --allow-fixed --dry-run --real --yes", "--fs --label"},
  {"extract", "--dry-run", ""},
  {"install-boot", "--allow-file --allow-fixed --dry-run --real --yes", "--mbr"},
  {"persist", "--dry-run", "--label --size"},
  {"badblocks", "--allow-file --yes", "--write-patterns"},
  {"mount", "--dry-run", ""},
  {"umount", "--dry-run", ""},
  {"secureboot-status", "", ""},
  {"validate-efi", "", ""},
  {"smart", "", ""},
  {"check-boot", "", ""},
  {"update-check", "", ""},
  {"download-windows", "--list --list-langs --url-only", "--arch --edition --lang --out --version"},
  {"create",
   "--allow-file --allow-fixed --dry-run --full --ignore-smart --no-autorun --quick --real --uefi-validate --verify --yes",
   "--badblock-passes --cluster-sectors --fs --keyboard --label --locale --mode --persist-mb --scheme "
   "--split-wim --timezone --wue"},
};

// 1 if `word` is one of the space-separated words in `list`.
static int in_list(const char *list, const char *word) {
  size_t n = strlen(word);
  for (const char *p = list; *p;) {
    while (*p == ' ') p++;
    const char *e = p;
    while (*e && *e != ' ') e++;
    if ((size_t)(e - p) == n && !strncmp(p, word, n)) return 1;
    p = e;
  }
  return 0;
}

static int edit_distance(const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  if (la > 40 || lb > 40) return 99;
  int d[41][41];
  for (size_t i = 0; i <= la; i++) d[i][0] = (int)i;
  for (size_t j = 0; j <= lb; j++) d[0][j] = (int)j;
  for (size_t i = 1; i <= la; i++)
    for (size_t j = 1; j <= lb; j++) {
      int c = a[i - 1] == b[j - 1] ? 0 : 1;
      int m = d[i - 1][j] + 1;
      if (d[i][j - 1] + 1 < m) m = d[i][j - 1] + 1;
      if (d[i - 1][j - 1] + c < m) m = d[i - 1][j - 1] + c;
      d[i][j] = m;
    }
  return d[la][lb];
}

// The closest known option to `bad`, or NULL if none is close enough.
static const char *closest_option(const CmdSpec *sp, const char *bad, char *buf, size_t cap) {
  int best = 3;
  const char *found = NULL;
  const char *lists[2] = {sp->flags, sp->values};
  for (int k = 0; k < 2; k++)
    for (const char *p = lists[k]; *p;) {
      while (*p == ' ') p++;
      const char *e = p;
      while (*e && *e != ' ') e++;
      if (e > p && (size_t)(e - p) < cap) {
        char w[64];
        snprintf(w, sizeof w, "%.*s", (int)(e - p), p);
        int d = edit_distance(bad, w);
        if (d < best) { best = d; snprintf(buf, cap, "%s", w); found = buf; }
      }
      p = e;
    }
  return found;
}

// Returns 0 if every option is known to the command and has its value, else 2.
static int validate_flags(int argc, char **argv) {
  const CmdSpec *sp = NULL;
  for (size_t k = 0; k < sizeof specs / sizeof specs[0]; k++)
    if (!strcmp(argv[1], specs[k].cmd)) sp = &specs[k];
  if (!sp) return 0;  // unknown command: the usage text handles it
  for (int i = 2; i < argc; i++) {
    const char *t = argv[i];
    if (t[0] != '-' || !t[1]) continue;  // a path or other positional argument
    if (in_list(sp->flags, t)) continue;
    if (in_list(sp->values, t)) {
      if (i + 1 >= argc) {
        fprintf(stderr, "rufux %s: option '%s' needs a value\n", sp->cmd, t);
        return 2;
      }
      i++;
      continue;
    }
    char buf[64];
    const char *g = closest_option(sp, t, buf, sizeof buf);
    fprintf(stderr, "rufux %s: unknown option '%s'", sp->cmd, t);
    if (g) fprintf(stderr, " (did you mean '%s'?)", g);
    fprintf(stderr, "\nNothing was changed. Run 'rufux' without arguments for the list of options.\n");
    return 2;
  }
  return 0;
}

// /dev/disk/by-id/usb-... and friends are symlinks. Every later step derives
// partition names from the real node (/dev/sdb -> /dev/sdb1), so resolve first.
static void resolve_device_links(int argc, char **argv) {
  for (int i = 2; i < argc; i++) {
    struct stat st;
    if (strncmp(argv[i], "/dev/", 5) || lstat(argv[i], &st) != 0 || !S_ISLNK(st.st_mode)) continue;
    char real[PATH_MAX];
    if (!realpath(argv[i], real)) continue;
    char *dup = strdup(real);
    if (!dup) continue;
    fprintf(stderr, "rufux: %s is %s\n", argv[i], dup);
    argv[i] = dup;
  }
}

int main(int argc, char **argv) {
  // Line-buffered stdout even into pipes: log lines must arrive live,
  // not in one block at exit (GUI streams them).
  setvbuf(stdout, NULL, _IOLBF, 0);
  rufux_i18n_init();
  if (argc >= 2 && (!strcmp(argv[1], "--gui") || !strcmp(argv[1], "gui")))
    return rufux_gui_run(argc, argv);
  if (argc >= 2) {
    if (validate_flags(argc, argv) != 0) return 2;
    resolve_device_links(argc, argv);
  }

  if (argc >= 2 && !strcmp(argv[1], "list")) {
    int json = 0, allow = 0;
    for (int i = 2; i < argc; i++) {
      if (!strcmp(argv[i], "--json")) json = 1;
      else if (!strcmp(argv[i], "--allow-fixed")) allow = 1;
    }
    RufuxDevice devs[128];
    int n = rufux_list_devices(devs, 128, allow);
    if (n < 0) { fprintf(stderr, "cannot scan /sys/block\n"); return 1; }
    if (json) rufux_print_devices_json(devs, n);
    else rufux_print_devices(devs, n);
    return 0;
  }
  if (argc >= 3 && !strcmp(argv[1], "probe")) {
    int detail = 0;
    for (int i = 3; i < argc; i++)
      if (!strcmp(argv[i], "--detail")) detail = 1;
    if (!detail) {
      char label[64] = {0};
      int rc = rufux_probe_iso(argv[2], label, sizeof label);
      printf("rc=%d label='%s'\n", rc, rc == 0 ? label : "?");
      return rc == 0 ? 0 : 2;
    }
    RufuxIsoInfo info;
    if (rufux_probe_iso_detail(argv[2], &info) != 0) {
      fprintf(stderr, "cannot probe '%s'\n", argv[2]);
      return 2;
    }
    rufux_print_iso_detail(argv[2], &info);
    return info.valid_iso ? 0 : 2;
  }
  if (argc >= 3 && !strcmp(argv[1], "checksum")) {
    RufuxHashAlg alg = RUFUX_SHA256;
    for (int i = 3; i < argc; i++) {
      if (!strcmp(argv[i], "--algo") && i + 1 < argc) {
        const char *a = argv[++i];
        if (!strcmp(a, "md5")) alg = RUFUX_MD5;
        else if (!strcmp(a, "sha1")) alg = RUFUX_SHA1;
        else if (!strcmp(a, "sha256")) alg = RUFUX_SHA256;
        else if (!strcmp(a, "sha512")) alg = RUFUX_SHA512;
        else { fprintf(stderr, "unknown algo '%s' (md5|sha1|sha256|sha512)\n", a); return 2; }
      }
    }
    unsigned char sum[64] = {0};
    unsigned len = 0;
    char err[256] = {0};
    if (rufux_hash_file(argv[2], alg, sum, &len, cli_progress, NULL, err, sizeof err) != 0) {
      fprintf(stderr, "checksum failed: %s\n", err);
      return 2;
    }
    char hex[129];
    rufux_hex(sum, len, hex);
    printf("%s  %s\n", hex, argv[2]);
    return 0;
  }
  if (argc >= 4 && !strcmp(argv[1], "write")) {
    RufuxWriteOpts o = {0};
    o.dry_run = 1;
    for (int i = 4; i < argc; i++) {
      if (!strcmp(argv[i], "--dry-run")) o.dry_run = 1;
      else if (!strcmp(argv[i], "--real")) o.dry_run = 0;
      else if (!strcmp(argv[i], "--verify")) o.verify = 1;
      else if (!strcmp(argv[i], "--allow-fixed")) o.allow_fixed = 1;
      else if (!strcmp(argv[i], "--allow-file")) o.allow_file = 1;
      else if (!strcmp(argv[i], "--yes")) o.yes = 1;
    }
    char err[512] = {0};
    if (!o.dry_run && rufux_need_root_for_block(argv[3], err, sizeof err) != 0) {
      fprintf(stderr, "write failed: %s\n", err);
      return 3;
    }
    {
      RufuxVdisk vd;
      int vk = rufux_vdisk_detect(argv[2], &vd, err, sizeof err);
      if (vk < 0) { fprintf(stderr, "write failed: %s\n", err); return 3; }
      if (vk > 0) {
        printf("%s image detected (%.1f GiB expanded): converting with qemu-img.\n",
               vd.label, vd.virtual_size / 1073741824.0);
        if (rufux_vdisk_write(argv[2], &vd, argv[3], &o, cli_progress, NULL, err, sizeof err) != 0) {
          fprintf(stderr, "write failed: %s\n", err[0] ? err : "unknown");
          return 3;
        }
        printf("%s OK: %s -> %s%s\n", o.dry_run ? "dry-run" : "write", argv[2], argv[3],
               o.verify ? " (verified)" : "");
        return 0;
      }
    }
    if (rufux_vhd_adjust(argv[2], &o, cli_clog, NULL, err, sizeof err) != 0) {
      fprintf(stderr, "write failed: %s\n", err);
      return 3;
    }
    int rc = rufux_write_image(argv[2], argv[3], &o, cli_progress, NULL, err, sizeof err);
    if (rc != 0) { fprintf(stderr, "write failed: %s\n", err[0] ? err : "unknown"); return 3; }
    printf("%s OK: %s -> %s%s\n", o.dry_run ? "dry-run" : "write",
           argv[2], argv[3], o.verify ? " (verified)" : "");
    return 0;
  }
  if (argc >= 3 && !strcmp(argv[1], "partition")) {
    RufuxPartOpts o = {.scheme = "gpt", .layout = "single", .dry_run = 1};
    for (int i = 3; i < argc; i++) {
      if (!strcmp(argv[i], "--scheme") && i + 1 < argc) o.scheme = argv[++i];
      else if (!strcmp(argv[i], "--layout") && i + 1 < argc) o.layout = argv[++i];
      else if (!strcmp(argv[i], "--fs") && i + 1 < argc) o.fs_main = argv[++i];
      else if (!strcmp(argv[i], "--dry-run")) o.dry_run = 1;
      else if (!strcmp(argv[i], "--real")) o.dry_run = 0;
      else if (!strcmp(argv[i], "--allow-file")) o.allow_file = 1;
      else if (!strcmp(argv[i], "--allow-fixed")) o.allow_fixed = 1;
      else if (!strcmp(argv[i], "--yes")) o.yes = 1;
    }
    char plan[512], err[512] = {0};
    rufux_partition_plan(argv[2], &o, plan, sizeof plan);
    printf("%s\n", plan);
    if (!o.dry_run && rufux_need_root_for_block(argv[2], err, sizeof err) != 0) {
      fprintf(stderr, "partition failed: %s\n", err);
      return 3;
    }
    if (rufux_partition(argv[2], &o, err, sizeof err) != 0) {
      fprintf(stderr, "partition failed: %s\n", err);
      return 3;
    }
    printf("partition %s\n", o.dry_run ? "planned (dry-run)" : "OK");
    return 0;
  }
  if (argc >= 3 && !strcmp(argv[1], "format")) {
    RufuxMkfsOpts o = {.fs = "vfat", .dry_run = 1};
    for (int i = 3; i < argc; i++) {
      if (!strcmp(argv[i], "--fs") && i + 1 < argc) o.fs = argv[++i];
      else if (!strcmp(argv[i], "--label") && i + 1 < argc) o.label = argv[++i];
      else if (!strcmp(argv[i], "--dry-run")) o.dry_run = 1;
      else if (!strcmp(argv[i], "--real")) o.dry_run = 0;
      else if (!strcmp(argv[i], "--allow-file")) o.allow_file = 1;
      else if (!strcmp(argv[i], "--allow-fixed")) o.allow_fixed = 1;
      else if (!strcmp(argv[i], "--yes")) o.yes = 1;
    }
    char err[512] = {0};
    printf("format %s as %s%s\n", argv[2], o.fs, o.dry_run ? " [dry-run]" : "");
    if (!o.dry_run && rufux_need_root_for_block(argv[2], err, sizeof err) != 0) {
      fprintf(stderr, "format failed: %s\n", err);
      return 3;
    }
    if (rufux_format(argv[2], &o, err, sizeof err) != 0) {
      fprintf(stderr, "format failed: %s\n", err);
      return 3;
    }
    printf("format %s\n", o.dry_run ? "planned (dry-run)" : "OK");
    return 0;
  }
  if (argc >= 4 && !strcmp(argv[1], "extract")) {
    int dry = 0;
    for (int i = 4; i < argc; i++)
      if (!strcmp(argv[i], "--dry-run")) dry = 1;
    char err[512] = {0};
    printf("extract %s -> %s%s\n", argv[2], argv[3], dry ? " [dry-run]" : "");
    if (rufux_extract_iso_progress(argv[2], argv[3], dry, cli_progress, NULL,
                                   err, sizeof err) != 0) {
      fprintf(stderr, "extract failed: %s\n", err);
      return 3;
    }
    printf("extract %s\n", dry ? "planned (dry-run)" : "OK");
    return 0;
  }
  if (argc >= 3 && !strcmp(argv[1], "install-boot")) {
    RufuxBootOpts o = {.kind = "bios", .dry_run = 1};
    for (int i = 3; i < argc; i++) {
      if (!strcmp(argv[i], "--mbr") && i + 1 < argc) o.kind = argv[++i];
      else if (!strcmp(argv[i], "--dry-run")) o.dry_run = 1;
      else if (!strcmp(argv[i], "--real")) o.dry_run = 0;
      else if (!strcmp(argv[i], "--allow-file")) o.allow_file = 1;
      else if (!strcmp(argv[i], "--allow-fixed")) o.allow_fixed = 1;
      else if (!strcmp(argv[i], "--yes")) o.yes = 1;
    }
    char err[512] = {0};
    printf("install-boot %s (%s)%s\n", argv[2], o.kind, o.dry_run ? " [dry-run]" : "");
    if (!o.dry_run && rufux_need_root_for_block(argv[2], err, sizeof err) != 0) {
      fprintf(stderr, "install-boot failed: %s\n", err);
      return 3;
    }
    if (rufux_install_mbr(argv[2], &o, err, sizeof err) != 0) {
      fprintf(stderr, "install-boot failed: %s\n", err);
      return 3;
    }
    printf("install-boot %s\n", o.dry_run ? "planned (dry-run)" : "OK");
    return 0;
  }
  if (argc >= 3 && !strcmp(argv[1], "persist")) {
    unsigned long mb = 0;
    const char *label = "casper-rw";
    int dry = 0;
    for (int i = 3; i < argc; i++) {
      if (!strcmp(argv[i], "--size") && i + 1 < argc) mb = strtoul(argv[++i], NULL, 10);
      else if (!strcmp(argv[i], "--label") && i + 1 < argc) label = argv[++i];
      else if (!strcmp(argv[i], "--dry-run")) dry = 1;
    }
    char err[512] = {0};
    if (rufux_create_persist(argv[2], label, mb, dry, err, sizeof err) != 0) {
      fprintf(stderr, "persist failed: %s\n", err);
      return 3;
    }
    printf("persist %s\n", dry ? "planned (dry-run)" : "OK");
    return 0;
  }
  if (argc >= 3 && !strcmp(argv[1], "badblocks")) {
    int allow_file = 0, wpasses = 0, yes = 0;
    for (int i = 3; i < argc; i++) {
      if (!strcmp(argv[i], "--allow-file")) allow_file = 1;
      else if (!strcmp(argv[i], "--write-patterns") && i + 1 < argc) wpasses = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--yes")) yes = 1;
    }
    char err[512] = {0};
    unsigned long long bad = 0;
    if (wpasses > 0) {
      // Destructive write-pattern test: needs confirmation + root.
      if (!yes) { fprintf(stderr, "badblocks: --write-patterns destroys data, add --yes\n"); return 2; }
      struct stat bst;
      if (stat(argv[2], &bst) == 0 && S_ISBLK(bst.st_mode) &&
          rufux_need_root_for_block(argv[2], err, sizeof err) != 0) {
        fprintf(stderr, "badblocks failed: %s\n", err);
        return 3;
      }
      if (rufux_badblocks_write(argv[2], allow_file, wpasses, 0,
                                cli_progress, NULL, &bad, err, sizeof err) != 0) {
        fprintf(stderr, "badblocks failed: %s\n", err);
        return 3;
      }
      printf("badblocks: %llu bad regions (0 = clean; %d write pattern pass(es), destructive)\n",
             bad, wpasses < 1 ? 1 : (wpasses > 4 ? 4 : wpasses));
      return bad == 0 ? 0 : 4;
    }
    if (rufux_badblocks(argv[2], allow_file, cli_progress, NULL, &bad, err, sizeof err) != 0) {
      fprintf(stderr, "badblocks failed: %s\n", err);
      return 3;
    }
    printf("badblocks: %llu bad regions (0 = clean; read-only surface scan)\n", bad);
    return bad == 0 ? 0 : 4;
  }
  if ((argc >= 3 && !strcmp(argv[1], "mount")) ||
      (argc >= 3 && !strcmp(argv[1], "umount"))) {
    int is_mount = !strcmp(argv[1], "mount");
    int dry = 0;
    for (int i = 3; i < argc; i++)
      if (!strcmp(argv[i], "--dry-run")) dry = 1;
    char err[512] = {0};
    if (is_mount) {
      char mnt[512] = {0};
      if (rufux_mount(argv[2], dry, mnt, sizeof mnt, err, sizeof err) != 0) {
        fprintf(stderr, "mount failed: %s\n", err);
        return 3;
      }
      printf("mounted %s at %s\n", argv[2], mnt);
    } else {
      if (rufux_unmount(argv[2], dry, err, sizeof err) != 0) {
        fprintf(stderr, "umount failed: %s\n", err);
        return 3;
      }
      printf("unmounted %s\n", argv[2]);
    }
    return 0;
  }
  if (argc >= 2 && !strcmp(argv[1], "secureboot-status")) {
    RufuxSbState s = rufux_sb_state();
    printf("secure-boot: %s\n", rufux_sb_string(s));
    return 0;
  }
  if (argc >= 3 && !strcmp(argv[1], "validate-efi")) {
    unsigned sub = 0;
    char err[512] = {0};
    if (rufux_validate_efi(argv[2], &sub, err, sizeof err) != 0) {
      fprintf(stderr, "validate-efi: %s\n", err);
      return 2;
    }
    printf("validate-efi: OK '%s' (PE subsystem %u = EFI)\n", argv[2], sub);
    return 0;
  }
  if (argc >= 3 && !strcmp(argv[1], "check-boot")) {
    struct stat cs;
    if (stat(argv[2], &cs) != 0) { fprintf(stderr, "check-boot: cannot open '%s'\n", argv[2]); return 2; }
    if (S_ISDIR(cs.st_mode)) return rufux_bootcheck_tree(argv[2], cli_clog, NULL) ? 4 : 0;
    RufuxBootCheck bc;
    if (rufux_bootcheck_file(argv[2], &bc) != 0) { fprintf(stderr, "check-boot: cannot read '%s'\n", argv[2]); return 2; }
    printf("%s: %s%s%s%s%s\n", argv[2], rufux_bootcheck_state_name(bc.state),
           bc.arch[0] ? " arch=" : "", bc.arch, bc.signer[0] ? " signer=" : "", bc.signer);
    if (bc.detail[0]) printf("  %s\n", bc.detail);
    return bc.state >= RUFUX_BOOT_REVOKED_DBX ? 4 : (bc.state == RUFUX_BOOT_NOTPE ? 2 : 0);
  }
  if (argc >= 3 && !strcmp(argv[1], "smart")) {
    char msg[256];
    RufuxSmartState st = rufux_smart_check(argv[2], msg, sizeof msg);
    printf("%s\n", msg);
    return st == RUFUX_SMART_FAILED ? 4 : 0;
  }
  if (argc >= 2 && !strcmp(argv[1], "update-check")) {
    char latest[64] = {0}, err[256] = {0};
    if (rufux_update_check(RUFUX_VERSION, latest, sizeof latest, err, sizeof err) != 0) {
      fprintf(stderr, "update-check: %s\n", err);
      return 5; // distinct code: skipped/offline
    }
    if (!strcmp(latest, RUFUX_VERSION))
      printf("rufux %s is up to date\n", RUFUX_VERSION);
    else
      printf("rufux %s installed, latest is %s. See https://github.com/Hultwl/Rufux/releases\n",
             RUFUX_VERSION, latest);
    return 0;
  }
  if (argc >= 4 && !strcmp(argv[1], "create")) {
    const char *src = argv[2], *dst = argv[3];
    RufuxCreateOpts o;
    rufux_create_defaults(&o);
    if (!strcmp(src, "none")) src = NULL;
    int fs_given = 0;
    for (int i = 4; i < argc; i++) {
      if (!strcmp(argv[i], "--mode") && i + 1 < argc) o.mode = argv[++i];
      else if (!strcmp(argv[i], "--scheme") && i + 1 < argc) o.scheme = argv[++i];
      else if (!strcmp(argv[i], "--fs") && i + 1 < argc) { o.fs = argv[++i]; fs_given = 1; }
      else if (!strcmp(argv[i], "--label") && i + 1 < argc) o.label = argv[++i];
      else if (!strcmp(argv[i], "--persist-mb") && i + 1 < argc) o.persist_mb = strtoul(argv[++i], NULL, 10);
      else if (!strcmp(argv[i], "--cluster-sectors") && i + 1 < argc) o.cluster_sectors = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--badblock-passes") && i + 1 < argc) o.badblock_passes = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--dry-run")) o.dry_run = 1;
      else if (!strcmp(argv[i], "--real")) o.dry_run = 0;
      else if (!strcmp(argv[i], "--allow-file")) o.allow_file = 1;
      else if (!strcmp(argv[i], "--allow-fixed")) o.allow_fixed = 1;
      else if (!strcmp(argv[i], "--yes")) o.yes = 1;
      else if (!strcmp(argv[i], "--verify")) o.verify = 1;
      else if (!strcmp(argv[i], "--quick")) o.quick_format = 1;
      else if (!strcmp(argv[i], "--full")) o.quick_format = 0;
      else if (!strcmp(argv[i], "--no-autorun")) o.extended_label = 0;
      else if (!strcmp(argv[i], "--uefi-validate")) o.uefi_validate = 1;
      else if (!strcmp(argv[i], "--ignore-smart")) o.ignore_smart = 1;
      else if (!strcmp(argv[i], "--wue") && i + 1 < argc) o.wue = argv[++i];
      else if (!strcmp(argv[i], "--split-wim") && i + 1 < argc) o.split_wim_mb = (unsigned)atoi(argv[++i]);
      else if (!strcmp(argv[i], "--locale") && i + 1 < argc) o.locale = argv[++i];
      else if (!strcmp(argv[i], "--keyboard") && i + 1 < argc) o.keyboard = argv[++i];
      else if (!strcmp(argv[i], "--timezone") && i + 1 < argc) o.timezone = argv[++i];
    }
    // Windows media keeps NTFS (+ UEFI:NTFS) as its default; FAT32 is opt-in with --fs vfat.
    if (!strcmp(o.mode, "windows") && !fs_given) o.fs = "ntfs";
    if ((!strcmp(o.mode, "dd") || !strcmp(o.mode, "extract") || !strcmp(o.mode, "windows")) && !src) {
      fprintf(stderr, "create: --mode %s needs an image (use 'none' only with --mode format|dos)\n", o.mode);
      return 2;
    }
    char err[1024] = {0};
    if (rufux_create(src, dst, &o, cli_progress, NULL, cli_clog, NULL, err, sizeof err) != 0) {
      fprintf(stderr, "create failed: %s\n", err[0] ? err : "unknown");
      return 3;
    }
    return 0;
  }
  // Double-click / app-grid behavior like upstream Rufus: with NO
  // arguments and a display available, open the GUI instead of usage.
  if (argc == 1 && (getenv("DISPLAY") || getenv("WAYLAND_DISPLAY")))
    return rufux_gui_run(argc, argv);
  if (argc >= 2 && !strcmp(argv[1], "download-windows")) {
    const char *ver = "11", *lang = NULL, *arch = NULL, *outdir = ".";
    int edition = 0, list = 0, list_langs = 0, url_only = 0;
    for (int i = 2; i < argc; i++) {
      if (!strcmp(argv[i], "--list")) list = 1;
      else if (!strcmp(argv[i], "--list-langs")) list_langs = 1;
      else if (!strcmp(argv[i], "--url-only")) url_only = 1;
      else if (!strcmp(argv[i], "--version") && i + 1 < argc) ver = argv[++i];
      else if (!strcmp(argv[i], "--edition") && i + 1 < argc) edition = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--lang") && i + 1 < argc) lang = argv[++i];
      else if (!strcmp(argv[i], "--arch") && i + 1 < argc) arch = argv[++i];
      else if (!strcmp(argv[i], "--out") && i + 1 < argc) outdir = argv[++i];
      else { fprintf(stderr, "download-windows: unexpected argument '%s'\n", argv[i]); return 2; }
    }
    int np;
    const RufuxMsdlProduct *pr = rufux_msdl_products(&np);
    if (list) {
      for (int i = 0; i < np; i++) {
        printf("%s  %s\n", pr[i].name, pr[i].release);
        for (int e = 0; e < pr[i].edition_count; e++) printf("  --edition %d  %s\n", e, pr[i].editions[e]);
      }
      return 0;
    }
    int pi = !strcmp(ver, "11") ? 0 : !strcmp(ver, "10") ? 1 : -1;
    if (pi < 0) { fprintf(stderr, "download-windows: --version must be 11 or 10\n"); return 2; }
    char err[600] = {0};
    RufuxMsdl *h = rufux_msdl_open(pi, edition, err, sizeof err);
    if (!h) { fprintf(stderr, "download-windows: %s\n", err); return 3; }
    if (list_langs) {
      for (int i = 0; i < rufux_msdl_language_count(h); i++)
        printf("%s|%s\n", rufux_msdl_language_name(h, i), rufux_msdl_language_display(h, i));
      rufux_msdl_close(h);
      return 0;
    }
    int li = rufux_msdl_find_language(h, lang, err, sizeof err);
    if (li < 0) { fprintf(stderr, "download-windows: %s\n", err); rufux_msdl_close(h); return 2; }
    RufuxMsdlLink links[4];
    int nl = rufux_msdl_links(h, li, links, 4, err, sizeof err);
    rufux_msdl_close(h);
    if (nl < 0) { fprintf(stderr, "download-windows: %s\n", err); return 3; }
    int pick = -1;
    if (arch) {
      for (int i = 0; i < nl; i++) if (!strcasecmp(links[i].arch, arch)) pick = i;
      if (pick < 0) {
        fprintf(stderr, "download-windows: no %s ISO for this edition. Available:", arch);
        for (int i = 0; i < nl; i++) fprintf(stderr, " %s", links[i].arch);
        fprintf(stderr, "\n");
        return 2;
      }
    } else {
      for (int i = 0; i < nl; i++) if (!strcmp(links[i].arch, "x64")) pick = i;
      if (pick < 0) pick = 0;
    }
    if (url_only) { printf("%s\n", links[pick].url); return 0; }
    printf("Downloading %s (%s)...\n", pr[pi].editions[edition], links[pick].arch);
    char iso[4200];
    if (rufux_msdl_fetch(links[pick].url, outdir, dl_progress, NULL, iso, sizeof iso, err, sizeof err) != 0) {
      fprintf(stderr, "download-windows: %s\n", err);
      return 3;
    }
    char hex[65];
    if (rufux_msdl_sha256(iso, hex) == 0) printf("SHA-256: %s\n", hex);
    printf("ISO: %s\n", iso);
    return 0;
  }
  usage(argv[0]);
  return 0;
}
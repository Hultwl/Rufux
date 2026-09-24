#define _GNU_SOURCE
#include "biosboot.h"
#include "exec.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

// Rescue-mode script: core.img runs it before any "normal" mode exists, so it
// is plain commands (no if/else). A failed `search` or `ntldr` prints an error
// and the next line is tried, which is what handles an upper-case BOOTMGR.
static const char early_cfg[] =
  "search --file --no-floppy --set=root /bootmgr\n"
  "ntldr /bootmgr\n"
  "boot\n"
  "search --file --no-floppy --set=root /BOOTMGR\n"
  "ntldr /BOOTMGR\n"
  "boot\n"
  "echo \"Rufux: bootmgr was not found on this drive.\"\n"
  "sleep 30\n";

static const char *const modules[] = {
  "biosdisk", "part_msdos", "part_gpt", "fat", "ntfs", "exfat", "ntldr",
  "search", "search_fs_file", "echo", "sleep", "boot", NULL};

static int is_file(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }

typedef struct {
  char moddir[PATH_MAX];  // holds boot.img, kernel.img and the .mod files
  char mkimage[PATH_MAX];
  char setup[PATH_MAX];
} Grub;

static int find_exe(const char *const *names, const char *const *dirs, char *out, size_t cap) {
  for (int i = 0; names[i]; i++) {
    if (rufux_have(names[i])) { snprintf(out, cap, "%s", names[i]); return 0; }
    for (int d = 0; dirs[d]; d++) {
      char p[PATH_MAX];
      snprintf(p, sizeof p, "%.2000s/%.2000s", dirs[d], names[i]);
      if (access(p, X_OK) == 0) { snprintf(out, cap, "%s", p); return 0; }
    }
  }
  return -1;
}

static int locate(Grub *g, char *why, unsigned long cap) {
  char ed[PATH_MAX], c[6][PATH_MAX];
  snprintf(ed, sizeof ed, "%s", rufux_exe_dir());
  int n = 0;
  const char *env = getenv("RUFUX_GRUB_DIR");
  if (env && env[0]) {
    // An explicit directory is used on its own: no fallback to the system's GRUB.
    snprintf(c[n++], PATH_MAX, "%.4000s", env);
  } else {
    if (ed[0]) snprintf(c[n++], PATH_MAX, "%.3900s/../lib/grub/i386-pc", ed);
    snprintf(c[n++], PATH_MAX, "/usr/lib/grub/i386-pc");
    snprintf(c[n++], PATH_MAX, "/usr/lib/grub2/i386-pc");
    snprintf(c[n++], PATH_MAX, "/usr/share/grub/i386-pc");
  }
  g->moddir[0] = 0;
  for (int i = 0; i < n; i++) {
    char t1[PATH_MAX + 16], t2[PATH_MAX + 16];
    snprintf(t1, sizeof t1, "%.4000s/boot.img", c[i]);
    snprintf(t2, sizeof t2, "%.4000s/kernel.img", c[i]);
    if (is_file(t1) && is_file(t2)) { snprintf(g->moddir, sizeof g->moddir, "%s", c[i]); break; }
  }
  if (!g->moddir[0]) {
    snprintf(why, cap, "the GRUB 2 BIOS modules are not installed (package grub-pc-bin / grub2-pc)");
    return -1;
  }
  const char *mk[] = {"grub-mkimage", "grub2-mkimage", NULL};
  const char *st[] = {"grub-bios-setup", "grub2-bios-setup", NULL};
  char d1[PATH_MAX + 16], d2[PATH_MAX + 16], d3[PATH_MAX + 16];
  snprintf(d1, sizeof d1, "%.4000s", g->moddir);
  snprintf(d2, sizeof d2, "%.3900s/../../bin", g->moddir);
  snprintf(d3, sizeof d3, "%.3900s/../../sbin", g->moddir);
  const char *dirs[] = {d1, d2, d3, "/usr/sbin", "/usr/bin", NULL};
  if (find_exe(mk, dirs, g->mkimage, sizeof g->mkimage) != 0) {
    snprintf(why, cap, "grub-mkimage is not installed (package grub-common / grub2-tools)");
    return -1;
  }
  if (find_exe(st, dirs, g->setup, sizeof g->setup) != 0) {
    snprintf(why, cap, "grub-bios-setup is not installed (package grub-pc-bin / grub2-pc)");
    return -1;
  }
  return 0;
}

int rufux_bios_available(char *why, unsigned long cap) {
  Grub g;
  why[0] = 0;
  return locate(&g, why, cap) == 0;
}

static int write_file(const char *path, const char *data, size_t n) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return -1;
  int ok = write(fd, data, n) == (ssize_t)n;
  close(fd);
  return ok ? 0 : -1;
}

static int copy_file(const char *from, const char *to) {
  FILE *a = fopen(from, "rb"), *b = a ? fopen(to, "wb") : NULL;
  if (!a || !b) { if (a) fclose(a); if (b) fclose(b); return -1; }
  char buf[65536];
  size_t n;
  int ok = 1;
  while ((n = fread(buf, 1, sizeof buf, a)) > 0) if (fwrite(buf, 1, n, b) != n) { ok = 0; break; }
  fclose(a);
  if (fclose(b) != 0) ok = 0;
  return ok ? 0 : -1;
}

static void rmtree_flat(const char *dir) {
  DIR *d = opendir(dir);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      char p[PATH_MAX];
      snprintf(p, sizeof p, "%.3000s/%.255s", dir, e->d_name);
      unlink(p);
    }
    closedir(d);
  }
  rmdir(dir);
}

// First non-empty MBR partition entry's start LBA (0 if none).
static unsigned first_part_lba(const unsigned char *mbr) {
  unsigned best = 0;
  for (int i = 0; i < 4; i++) {
    const unsigned char *e = mbr + 446 + i * 16;
    unsigned lba = (unsigned)(e[8] | e[9] << 8 | e[10] << 16 | (unsigned)e[11] << 24);
    unsigned sz = (unsigned)(e[12] | e[13] << 8 | e[14] << 16 | (unsigned)e[15] << 24);
    if (e[4] && sz && (!best || lba < best)) best = lba;
  }
  return best;
}

static int read_mbr(const char *disk, unsigned char *mbr) {
  int fd = open(disk, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  int ok = pread(fd, mbr, 512, 0) == 512;
  close(fd);
  return ok ? 0 : -1;
}

int rufux_windows_bios_install(const char *disk, const char *mnt, RufuxBiosLog log, void *lu,
                               char *err, unsigned long cap) {
  Grub g;
  if (locate(&g, err, cap) != 0) return -1;
  unsigned char before[512], after[512];
  if (read_mbr(disk, before) != 0 || before[510] != 0x55 || before[511] != 0xAA) {
    snprintf(err, cap, "'%s' has no MBR partition table (legacy BIOS boot needs the MBR scheme)", disk);
    return -1;
  }
  // The scratch directory lives on the target partition: grub-bios-setup works out the
  // device it is installing for from where its files sit, and it cannot do that for /tmp.
  char work[PATH_MAX], core[PATH_MAX + 16], cfg[PATH_MAX + 16], devmap[PATH_MAX + 16], boot_img[PATH_MAX + 16], mk_out[PATH_MAX + 16];
  snprintf(work, sizeof work, "%.4000s/.rufux-grub", mnt);
  if (mkdir(work, 0700) != 0 && errno != EEXIST) {
    snprintf(err, cap, "cannot create %s: %s", work, strerror(errno));
    return -1;
  }
  snprintf(core, sizeof core, "%.4000s/core.img", work);
  snprintf(cfg, sizeof cfg, "%.4000s/early.cfg", work);
  snprintf(devmap, sizeof devmap, "%.4000s/device.map", work);
  snprintf(boot_img, sizeof boot_img, "%.4000s/boot.img", work);
  snprintf(mk_out, sizeof mk_out, "%s", core);
  int rc = -1;
  char devline[PATH_MAX + 16];
  snprintf(devline, sizeof devline, "(hd0) %.4000s\n", disk);
  char src_boot[PATH_MAX + 16];
  snprintf(src_boot, sizeof src_boot, "%.4000s/boot.img", g.moddir);
  if (write_file(cfg, early_cfg, sizeof early_cfg - 1) != 0 || write_file(devmap, devline, strlen(devline)) != 0 ||
      copy_file(src_boot, boot_img) != 0) {
    snprintf(err, cap, "cannot write GRUB files to %s: %s", work, strerror(errno));
    goto out;
  }
  const char *mk[6 + 16 + 1];
  int n = 0;
  mk[n++] = g.mkimage; mk[n++] = "-O"; mk[n++] = "i386-pc"; mk[n++] = "-d"; mk[n++] = g.moddir;
  mk[n++] = "-o"; mk[n++] = core; mk[n++] = "-c"; mk[n++] = cfg; mk[n++] = "-p"; mk[n++] = "/.rufux-grub";
  for (int i = 0; modules[i]; i++) mk[n++] = modules[i];
  mk[n] = NULL;
  {
    // Passed as a NULL-terminated argv; rufux_capture merges stderr so failures are explained.
    char out[1024];
    if (rufux_capture(mk, out, sizeof out) != 0 || !is_file(core)) {
      char *nl = strchr(out, '\n');
      if (nl) *nl = 0;
      snprintf(err, cap, "grub-mkimage failed: %.300s", out[0] ? out : "unknown error");
      goto out;
    }
  }
  struct stat cs;
  stat(core, &cs);
  unsigned need = (unsigned)((cs.st_size + 511) / 512) + 2;
  unsigned first = first_part_lba(before);
  if (first && first < need) {
    snprintf(err, cap, "the gap before the first partition (%u sectors) is too small for the boot code (%u needed)",
             first - 1, need - 1);
    goto out;
  }
  const char *su[] = {g.setup, "-d", work, "-b", "boot.img", "-c", "core.img", "-m", devmap,
                      "--skip-fs-probe", "--force", disk, NULL};
  {
    char out[1024];
    if (rufux_capture(su, out, sizeof out) != 0) {
      char *nl = strchr(out, '\n');
      if (nl) *nl = 0;
      snprintf(err, cap, "grub-bios-setup failed: %.300s", out[0] ? out : "unknown error");
      goto out;
    }
  }
  sync();
  // The partition table must survive and the drive must now start with boot code.
  if (read_mbr(disk, after) != 0 || after[510] != 0x55 || after[511] != 0xAA ||
      memcmp(before + 446, after + 446, 66) != 0 || after[0] == 0) {
    snprintf(err, cap, "boot code installation left the MBR in an unexpected state; not trusting it");
    goto out;
  }
  rc = 0;
  if (log) log("Legacy BIOS boot code installed (GRUB 2 in the MBR gap; it starts bootmgr).", lu);
out:
  rmtree_flat(work);
  return rc;
}

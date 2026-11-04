#include "boot.h"
#include "device.h"
#include "exec.h"
#include "platform_macos.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Upstream ms-sys compiled-in Syslinux 6.02 MBR bootstrap arrays (440 bytes each).
#include "../../src/ms-sys/inc/mbr_gpt_syslinux.h"
#include "../../src/ms-sys/inc/mbr_syslinux.h"

static int is_valid_mbr_bin(const char *p) {
  struct stat st;
  return p && p[0] && stat(p, &st) == 0 && S_ISREG(st.st_mode) &&
         st.st_size >= 440 && access(p, R_OK) == 0;
}

static int find_external_mbr_file(const char *kind, char *out, size_t cap) {
  const char *base = (kind && strcmp(kind, "gpt") == 0) ? "gptmbr.bin" : "mbr.bin";
  char cand[PATH_MAX * 2];

  const char *res_env = getenv("RUFUX_RES");
  if (res_env && res_env[0]) {
    snprintf(cand, sizeof cand, "%s/syslinux/%s", res_env, base);
    if (is_valid_mbr_bin(cand)) {
      snprintf(out, cap, "%s", cand);
      return 1;
    }
  }

  const char *ed = rufux_exe_dir();
  if (ed && ed[0]) {
    const char *rels[] = {
      "../Resources/share/rufux/syslinux",
      "../../Resources/share/rufux/syslinux",
      "../share/rufux/syslinux",
      "../Resources/share/syslinux",
      "../share/syslinux",
      NULL
    };
    for (int i = 0; rels[i]; i++) {
      snprintf(cand, sizeof cand, "%s/%s/%s", ed, rels[i], base);
      if (is_valid_mbr_bin(cand)) {
        snprintf(out, cap, "%s", cand);
        return 1;
      }
    }
  }

  const char *sysdirs[] = {
    "/opt/homebrew/share/syslinux",
    "/usr/local/share/syslinux",
    "/usr/share/syslinux/bios",
    "/usr/share/syslinux",
    "/usr/lib/syslinux/bios",
    "/usr/lib/syslinux/mbr",
    "/usr/lib/syslinux",
    NULL
  };
  for (int i = 0; sysdirs[i]; i++) {
    snprintf(cand, sizeof cand, "%s/%s", sysdirs[i], base);
    if (is_valid_mbr_bin(cand)) {
      snprintf(out, cap, "%s", cand);
      return 1;
    }
  }
  return 0;
}

// Loads 440-byte Syslinux BIOS or GPT MBR bootstrap from bundled/system file if present,
// falling back directly to compiled-in ms-sys arrays (mbr_syslinux_0x0 / mbr_gpt_syslinux_0x0).
int rufux_boot_load_mbr440(const char *kind, unsigned char out[440],
                           char *src_desc, size_t desc_cap) {
  int is_gpt = (kind && strcmp(kind, "gpt") == 0);
  char ext_path[PATH_MAX] = {0};
  if (find_external_mbr_file(kind, ext_path, sizeof ext_path)) {
    int fd = open(ext_path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
      ssize_t got = rufux_read_full(fd, out, 440);
      close(fd);
      if (got == 440) {
        if (src_desc && desc_cap > 0) snprintf(src_desc, desc_cap, "%s", ext_path);
        return 0;
      }
    }
  }

  memset(out, 0, 440);
  if (is_gpt) {
    size_t n = sizeof mbr_gpt_syslinux_0x0 < 440 ? sizeof mbr_gpt_syslinux_0x0 : 440;
    memcpy(out, mbr_gpt_syslinux_0x0, n);
    if (src_desc && desc_cap > 0) snprintf(src_desc, desc_cap, "builtin:gptmbr.bin");
  } else {
    size_t n = sizeof mbr_syslinux_0x0 < 440 ? sizeof mbr_syslinux_0x0 : 440;
    memcpy(out, mbr_syslinux_0x0, n);
    if (src_desc && desc_cap > 0) snprintf(src_desc, desc_cap, "builtin:mbr.bin");
  }
  return 0;
}

static int pread_exact(int fd, void *buf, size_t len, off_t off) {
  unsigned char *p = (unsigned char *)buf;
  size_t got = 0;
  while (got < len) {
    ssize_t r = pread(fd, p + got, len - got, off + (off_t)got);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return -1;
    got += (size_t)r;
  }
  return 0;
}

int rufux_install_mbr(const char *dst, const RufuxBootOpts *o,
                      char *err, unsigned long cap) {
  if (err && cap > 0) err[0] = 0;
  if (!dst || !dst[0] || !o) {
    if (err && cap > 0) snprintf(err, cap, "invalid MBR install arguments");
    return -1;
  }
  if (o->kind && o->kind[0] &&
      strcmp(o->kind, "bios") != 0 && strcmp(o->kind, "gpt") != 0) {
    if (err && cap > 0)
      snprintf(err, cap, "unknown MBR kind '%s' (expected bios|gpt)", o->kind);
    return -1;
  }
  if (!o->dry_run && !o->yes) {
    if (err && cap > 0) snprintf(err, cap, "refusing real MBR write without --yes");
    return -1;
  }
  if (rufux_check_target(dst, o->allow_fixed, o->allow_file, err, cap) != 0)
    return -1;

  unsigned char code[440];
  char src_desc[PATH_MAX] = {0};
  rufux_boot_load_mbr440(o->kind, code, src_desc, sizeof src_desc);

  if (o->dry_run) {
    fprintf(stderr, "+ dd MBR %s -> %s (first 440 bytes, table preserved)\n", src_desc, dst);
    return 0;
  }

  char bnode[PATH_MAX];
  rufux_macos_block_node(dst, bnode, sizeof bnode);

  struct stat st;
  if (stat(bnode, &st) != 0) {
    if (err && cap > 0) snprintf(err, cap, "cannot stat '%s': %s", bnode, strerror(errno));
    return -1;
  }
  int is_dev = S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode);

  int fd = open(bnode, O_RDWR | O_CLOEXEC);
  if (fd < 0 && errno == EBUSY && is_dev) {
    if (rufux_macos_unmount_disk(bnode, err, cap) != 0) return -1;
    fd = open(bnode, O_RDWR | O_CLOEXEC);
  }
  if (fd < 0) {
    if (err && cap > 0) snprintf(err, cap, "cannot open '%s': %s", bnode, strerror(errno));
    return -1;
  }

  int rc = 0;
  if (is_dev) {
    unsigned sec_sz = rufux_macos_sector_size(bnode);
    if (sec_sz < 512u || sec_sz > 4096u) sec_sz = 512u;
    unsigned char sec[4096] = {0};
    if (pread_exact(fd, sec, sec_sz, 0) != 0) {
      if (err && cap > 0) snprintf(err, cap, "cannot read MBR sector from '%s': %s", bnode, strerror(errno));
      rc = -1;
    } else {
      memcpy(sec, code, sizeof code);
      if (rufux_pwrite_all(fd, sec, sec_sz, 0) != 0) {
        if (err && cap > 0) snprintf(err, cap, "MBR write failed on '%s': %s", bnode, strerror(errno));
        rc = -1;
      }
    }
  } else {
    if (rufux_pwrite_all(fd, code, sizeof code, 0) != 0) {
      if (err && cap > 0) snprintf(err, cap, "MBR write failed on '%s': %s", bnode, strerror(errno));
      rc = -1;
    }
  }

  if (rc == 0 && fsync(fd) != 0) {
    if (err && cap > 0) snprintf(err, cap, "fsync failed on '%s': %s", bnode, strerror(errno));
    rc = -1;
  }
  close(fd);
  return rc;
}

int rufux_install_syslinux(const char *part_dev, int dry_run,
                           char *err, unsigned long cap) {
  if (err && cap > 0) err[0] = 0;
  if (!rufux_have("syslinux")) {
    if (err && cap > 0) snprintf(err, cap, "syslinux missing");
    return -1;
  }
  const char *av[] = {"syslinux", "--install", part_dev, NULL};
  if (rufux_run(av, dry_run) != 0) {
    if (!dry_run && err && cap > 0)
      snprintf(err, cap, "syslinux --install failed on '%s'", part_dev);
    return dry_run ? 0 : -1;
  }
  return 0;
}

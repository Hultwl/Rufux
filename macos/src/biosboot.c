#include "biosboot.h"
#include "exec.h"
#include "platform_macos.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
  "search", "search_fs_file", "echo", "sleep", "boot", NULL
};

static int is_file(const char *p) {
  struct stat st;
  return p && p[0] && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static uint32_t get_le32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const unsigned char *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

static void put_le16(unsigned char *p, uint16_t v) {
  p[0] = (unsigned char)(v & 0xFFu);
  p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static void put_le64(unsigned char *p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = (unsigned char)((v >> (i * 8)) & 0xFFu);
}

typedef struct {
  char moddir[PATH_MAX];
  char mkimage[PATH_MAX];
} Grub;

static int has_grub_modules(const char *dir) {
  if (!dir || !dir[0]) return 0;
  char t1[PATH_MAX * 2], t2[PATH_MAX * 2], t3[PATH_MAX * 2];
  snprintf(t1, sizeof t1, "%s/boot.img", dir);
  snprintf(t2, sizeof t2, "%s/diskboot.img", dir);
  snprintf(t3, sizeof t3, "%s/kernel.img", dir);
  return is_file(t1) && is_file(t2) && is_file(t3);
}

static int locate(Grub *g, char *why, unsigned long cap) {
  memset(g, 0, sizeof *g);

  const char *explicit_env = getenv("RUFUX_GRUB_DIR");
  if (explicit_env && explicit_env[0]) {
    if (has_grub_modules(explicit_env)) {
      snprintf(g->moddir, sizeof g->moddir, "%s", explicit_env);
    } else {
      if (why && cap > 0)
        snprintf(why, cap, "RUFUX_GRUB_DIR '%s' is missing boot.img/diskboot.img/kernel.img", explicit_env);
      return -1;
    }
  } else {
    rufux_macos_init_bundle_env();
    const char *benv = getenv("RUFUX_GRUB_DIR");
    if (benv && benv[0] && has_grub_modules(benv)) {
      snprintf(g->moddir, sizeof g->moddir, "%s", benv);
    }
  }

  if (!g->moddir[0]) {
    const char *ed = rufux_macos_exe_dir();
    if (ed && ed[0]) {
      const char *rels[] = {
        "../Resources/lib/grub/i386-pc",
        "../../Resources/lib/grub/i386-pc",
        "../lib/grub/i386-pc",
        NULL
      };
      for (int i = 0; rels[i] && !g->moddir[0]; i++) {
        char cand[PATH_MAX * 2];
        snprintf(cand, sizeof cand, "%s/%s", ed, rels[i]);
        if (has_grub_modules(cand)) {
          char resolved[PATH_MAX];
          snprintf(g->moddir, sizeof g->moddir, "%s",
                   realpath(cand, resolved) ? resolved : cand);
        }
      }
    }
  }

  if (!g->moddir[0]) {
    const char *sysdirs[] = {
      "/opt/homebrew/opt/i686-elf-grub/lib/i686-elf/grub/i386-pc",
      "/opt/homebrew/lib/i686-elf/grub/i386-pc",
      "/opt/homebrew/lib/i686-elf-grub/i386-pc",
      "/usr/local/lib/i686-elf/grub/i386-pc",
      NULL
    };
    for (int i = 0; sysdirs[i]; i++) {
      if (has_grub_modules(sysdirs[i])) {
        snprintf(g->moddir, sizeof g->moddir, "%s", sysdirs[i]);
        break;
      }
    }
  }

  if (!g->moddir[0]) {
    if (why && cap > 0)
      snprintf(why, cap, "GRUB 2 i386-pc modules not found (brew install i686-elf-grub)");
    return -1;
  }

  const char *resolved_mk = rufux_resolve_tool("grub-mkimage");
  if (resolved_mk) {
    snprintf(g->mkimage, sizeof g->mkimage, "%s", resolved_mk);
    return 0;
  }

  const char *fallbacks[] = {
    "/opt/homebrew/opt/i686-elf-grub/bin/i686-elf-grub-mkimage",
    "/opt/homebrew/bin/i686-elf-grub-mkimage",
    "/usr/local/bin/i686-elf-grub-mkimage",
    NULL
  };
  for (int i = 0; fallbacks[i]; i++) {
    if (access(fallbacks[i], X_OK) == 0) {
      snprintf(g->mkimage, sizeof g->mkimage, "%s", fallbacks[i]);
      return 0;
    }
  }

  if (why && cap > 0)
    snprintf(why, cap, "i686-elf-grub-mkimage is not installed (brew install i686-elf-grub)");
  return -1;
}

int rufux_bios_available(char *why, unsigned long cap) {
  Grub g;
  if (why && cap > 0) why[0] = 0;
  return locate(&g, why, cap) == 0;
}

static int pread_exact_nocache(const char *path, void *buf, size_t n, off_t off) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  (void)fcntl(fd, F_NOCACHE, 1);
  unsigned char *p = (unsigned char *)buf;
  size_t got = 0;
  while (got < n) {
    ssize_t r = pread(fd, p + got, n - got, off + (off_t)got);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) { close(fd); return -1; }
    got += (size_t)r;
  }
  close(fd);
  return 0;
}

static int write_text_file(const char *path, const char *data, size_t n) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return -1;
  int ok = rufux_pwrite_all(fd, data, n, 0) == 0;
  close(fd);
  return ok ? 0 : -1;
}

static void rmtree_flat(const char *dir) {
  DIR *d = opendir(dir);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      char p[PATH_MAX * 2];
      snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
      (void)unlink(p);
    }
    closedir(d);
  }
  (void)rmdir(dir);
}

static uint32_t first_part_lba(const unsigned char *mbr) {
  uint32_t best = 0;
  for (int i = 0; i < 4; i++) {
    const unsigned char *e = mbr + 446 + i * 16;
    uint32_t lba = get_le32(e + 8);
    uint32_t sz = get_le32(e + 12);
    if (e[4] && sz && (!best || lba < best)) best = lba;
  }
  return best;
}

static int is_gpt(const unsigned char *mbr) {
  return mbr[446 + 4] == 0xEE;
}

static uint64_t gpt_bios_boot(const char *bnode, unsigned sec_sz, uint64_t *sectors,
                              char *err, unsigned long cap) {
  // 21686148-6449-6E6F-744E-656564454649 ("Hah!IdontNeedEFI" in mixed-endian GPT order)
  static const unsigned char guid[16] = {
    0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6F, 0x6E,
    0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49
  };
  unsigned char h[512];
  if (pread_exact_nocache(bnode, h, sizeof h, (off_t)sec_sz) != 0 ||
      memcmp(h, "EFI PART", 8) != 0) {
    if (err && cap > 0) snprintf(err, cap, "'%s' has no readable GPT header at LBA 1", bnode);
    return 0;
  }
  uint64_t ent = get_le64(h + 72);
  uint32_t num = get_le32(h + 80), esz = get_le32(h + 84);
  if (ent < 2 || num == 0 || esz < 128 || esz > 4096) {
    if (err && cap > 0) snprintf(err, cap, "'%s' has a malformed GPT header", bnode);
    return 0;
  }
  size_t total = (size_t)num * (size_t)esz;
  if (total > (1u << 20)) total = 1u << 20;
  unsigned char *ents = malloc(total);
  if (!ents) {
    if (err && cap > 0) snprintf(err, cap, "out of memory");
    return 0;
  }
  uint64_t found = 0;
  if (pread_exact_nocache(bnode, ents, total, (off_t)(ent * sec_sz)) == 0) {
    for (uint32_t i = 0; (size_t)(i + 1) * (size_t)esz <= total; i++) {
      const unsigned char *e = ents + (size_t)i * (size_t)esz;
      if (!memcmp(e, guid, sizeof guid)) {
        found = get_le64(e + 32);
        uint64_t last = get_le64(e + 40);
        if (last >= found) *sectors = (last - found + 1ULL) * (sec_sz / 512ULL);
        else found = 0;
        break;
      }
    }
  }
  free(ents);
  if (!found && err && cap > 0) {
    snprintf(err, cap, "'%s' has no BIOS boot partition, so it cannot start on legacy BIOS", bnode);
  }
  return found;
}

int rufux_windows_bios_install(const char *disk, const char *mnt,
                               RufuxBiosLog log, void *lu,
                               char *err, unsigned long cap) {
  (void)mnt;
  if (err && cap > 0) err[0] = 0;
  if (!disk || !disk[0]) {
    if (err && cap > 0) snprintf(err, cap, "empty disk path");
    return -1;
  }
  if (rufux_macos_is_boot_disk(disk)) {
    if (err && cap > 0) snprintf(err, cap, "refusing boot/system disk '%s'", disk);
    return -1;
  }

  Grub g;
  if (locate(&g, err, cap) != 0) return -1;

  char bnode[PATH_MAX];
  rufux_macos_block_node(disk, bnode, sizeof bnode);

  struct stat dst_st;
  if (stat(bnode, &dst_st) != 0) {
    if (err && cap > 0) snprintf(err, cap, "cannot stat '%s': %s", bnode, strerror(errno));
    return -1;
  }
  int is_dev = S_ISBLK(dst_st.st_mode) || S_ISCHR(dst_st.st_mode);
  unsigned sec_sz = is_dev ? rufux_macos_sector_size(bnode) : 512u;
  if (sec_sz < 512u || sec_sz > 4096u || (sec_sz % 512u) != 0) sec_sz = 512u;

  unsigned char before[4096] = {0}, after[4096] = {0}, boot_buf[512] = {0};
  if (pread_exact_nocache(bnode, before, sec_sz, 0) != 0 ||
      before[510] != 0x55 || before[511] != 0xAA) {
    if (err && cap > 0) snprintf(err, cap, "'%s' has no valid MBR/protective-MBR signature", bnode);
    return -1;
  }

  char src_boot[PATH_MAX * 2];
  snprintf(src_boot, sizeof src_boot, "%s/boot.img", g.moddir);
  if (pread_exact_nocache(src_boot, boot_buf, sizeof boot_buf, 0) != 0 ||
      boot_buf[510] != 0x55 || boot_buf[511] != 0xAA) {
    if (err && cap > 0) snprintf(err, cap, "invalid GRUB boot.img at '%s'", src_boot);
    return -1;
  }

  int gpt = is_gpt(before);
  uint64_t core_lba = 1ULL;
  uint64_t avail_sectors = 0ULL;
  if (gpt) {
    uint64_t part_lba_native = gpt_bios_boot(bnode, sec_sz, &avail_sectors, err, cap);
    if (part_lba_native == 0) return -1;
    core_lba = part_lba_native * (sec_sz / 512ULL);
  } else {
    uint32_t first_native = first_part_lba(before);
    if (first_native == 0) {
      if (err && cap > 0)
        snprintf(err, cap, "'%s' has no MBR partitions", bnode);
      return -1;
    }
    uint64_t first_512 = (uint64_t)first_native * (sec_sz / 512ULL);
    if (first_512 <= 1ULL) {
      if (err && cap > 0)
        snprintf(err, cap,
                 "MBR gap before partition 1 (0 sectors; partition 1 starts at LBA %llu) is too small for core.img",
                 (unsigned long long)first_512);
      return -1;
    }
    avail_sectors = first_512 - 1ULL;
  }

  char work[] = "/tmp/rufux-grub-XXXXXX";
  if (!mkdtemp(work)) {
    if (err && cap > 0) snprintf(err, cap, "mkdtemp failed: %s", strerror(errno));
    return -1;
  }

  int rc = -1;
  unsigned char *core_buf = NULL;
  char cfg[PATH_MAX], core_path[PATH_MAX];
  snprintf(cfg, sizeof cfg, "%s/early.cfg", work);
  snprintf(core_path, sizeof core_path, "%s/core.img", work);

  if (write_text_file(cfg, early_cfg, sizeof early_cfg - 1) != 0) {
    if (err && cap > 0) snprintf(err, cap, "cannot write '%s': %s", cfg, strerror(errno));
    goto out;
  }

  const char *mk[32];
  int n = 0;
  mk[n++] = g.mkimage;
  mk[n++] = "-O";
  mk[n++] = "i386-pc";
  mk[n++] = "-d";
  mk[n++] = g.moddir;
  mk[n++] = "-c";
  mk[n++] = cfg;
  mk[n++] = "-o";
  mk[n++] = core_path;
  mk[n++] = "-p";
  mk[n++] = "";
  for (int i = 0; modules[i]; i++) mk[n++] = modules[i];
  mk[n] = NULL;

  {
    char out[1024] = {0};
    if (rufux_capture(mk, out, sizeof out) != 0 || !is_file(core_path)) {
      char *nl = strchr(out, '\n');
      if (nl) *nl = 0;
      if (err && cap > 0)
        snprintf(err, cap, "grub-mkimage failed: %.300s", out[0] ? out : "unknown error");
      goto out;
    }
  }

  struct stat cs;
  if (stat(core_path, &cs) != 0 || cs.st_size < 1024 || cs.st_size > (1024 * 1024)) {
    if (err && cap > 0) snprintf(err, cap, "generated core.img has invalid size (%lld bytes)",
                                 (long long)cs.st_size);
    goto out;
  }

  uint64_t core_sectors = ((uint64_t)cs.st_size + 511ULL) / 512ULL;
  if (core_sectors < 2ULL || core_sectors > 0xFFFFULL) {
    if (err && cap > 0) snprintf(err, cap, "core.img sector count (%llu) out of range",
                                 (unsigned long long)core_sectors);
    goto out;
  }

  if (gpt) {
    if (avail_sectors < core_sectors) {
      if (err && cap > 0)
        snprintf(err, cap,
                 "BIOS boot partition at LBA %llu is %llu sectors, too small for core.img (%llu needed)",
                 (unsigned long long)core_lba, (unsigned long long)avail_sectors,
                 (unsigned long long)core_sectors);
      goto out;
    }
  } else {
    if (avail_sectors < core_sectors) {
      if (err && cap > 0)
        snprintf(err, cap,
                 "MBR gap before partition 1 (%llu sectors) is too small for core.img (%llu needed)",
                 (unsigned long long)avail_sectors, (unsigned long long)core_sectors);
      goto out;
    }
  }

  size_t raw_write_bytes = (size_t)(core_sectors * 512ULL);
  uint64_t byte_off = core_lba * 512ULL;
  uint64_t aligned_off = (byte_off / sec_sz) * sec_sz;
  size_t lead_pad = (size_t)(byte_off - aligned_off);
  size_t total_span = ((lead_pad + raw_write_bytes + sec_sz - 1u) / sec_sz) * sec_sz;
  core_buf = calloc(1, total_span);
  if (!core_buf) {
    if (err && cap > 0) snprintf(err, cap, "out of memory");
    goto out;
  }

  if (lead_pad > 0 || total_span > lead_pad + raw_write_bytes) {
    (void)pread_exact_nocache(bnode, core_buf, total_span, (off_t)aligned_off);
    memset(core_buf + lead_pad, 0, raw_write_bytes);
  }
  unsigned char *core_img = core_buf + lead_pad;

  {
    int cfd = open(core_path, O_RDONLY | O_CLOEXEC);
    if (cfd < 0) {
      if (err && cap > 0) snprintf(err, cap, "cannot open core.img: %s", strerror(errno));
      goto out;
    }
    ssize_t got = rufux_read_full(cfd, core_img, (size_t)cs.st_size);
    close(cfd);
    if (got != (ssize_t)cs.st_size) {
      if (err && cap > 0) snprintf(err, cap, "short read of core.img");
      goto out;
    }
  }

  // Patch diskboot.img (first 512 bytes of core.img):
  // 488..499 (0x1E8): terminating 12-byte zero blocklist entry
  // 500..511 (0x1F4): start_lba = core_lba + 1, count = core_sectors - 1, seg = 0x0820
  memset(core_img + 488, 0, 12);
  put_le64(core_img + 500, core_lba + 1ULL);
  put_le16(core_img + 508, (uint16_t)(core_sectors - 1ULL));
  put_le16(core_img + 510, 0x0820u);

  // Build patched MBR preserving bytes 440..511 (disk signature + partition table + 0x55AA).
  unsigned char new_mbr[4096];
  memcpy(new_mbr, before, sec_sz);
  memcpy(new_mbr, boot_buf, 440);
  put_le64(new_mbr + 0x5c, core_lba);
  new_mbr[0x64] = 0xFF;
  new_mbr[0x66] = 0x90;
  new_mbr[0x67] = 0x90;

  {
    int fd = open(bnode, O_RDWR | O_CLOEXEC);
    if (fd < 0 && errno == EBUSY && is_dev) {
      char uerr[256];
      (void)rufux_macos_unmount_disk(bnode, uerr, sizeof uerr);
      usleep(100000);
      fd = open(bnode, O_RDWR | O_CLOEXEC);
    }
    if (fd < 0) {
      if (err && cap > 0) snprintf(err, cap, "cannot open '%s' for BIOS boot install: %s",
                                   bnode, strerror(errno));
      goto out;
    }
    (void)fcntl(fd, F_NOCACHE, 1);

    if (aligned_off == 0 && lead_pad >= 512u) {
      // sec_sz > 512 with core_lba == 1: merge patched MBR (0..511) into core_buf[0..511]
      // so a single sec_sz-aligned write commits both without clobbering core.img bytes 512..sec_sz-1.
      memcpy(core_buf, new_mbr, 512);
      if (rufux_pwrite_all(fd, core_buf, total_span, 0) != 0) {
        if (err && cap > 0) snprintf(err, cap, "writing GRUB MBR + core.img failed: %s", strerror(errno));
        close(fd);
        goto out;
      }
    } else {
      if (rufux_pwrite_all(fd, new_mbr, sec_sz, 0) != 0) {
        if (err && cap > 0) snprintf(err, cap, "writing GRUB boot.img MBR failed: %s", strerror(errno));
        close(fd);
        goto out;
      }
      if (rufux_pwrite_all(fd, core_buf, total_span, (off_t)aligned_off) != 0) {
        if (err && cap > 0) snprintf(err, cap, "writing GRUB core.img failed: %s", strerror(errno));
        close(fd);
        goto out;
      }
    }
    if (fsync(fd) != 0) {
      if (err && cap > 0) snprintf(err, cap, "fsync failed on '%s': %s", bnode, strerror(errno));
      close(fd);
      goto out;
    }
    close(fd);
  }

  // Verify MBR signature + disk signature + partition table + GPT header integrity.
  if (pread_exact_nocache(bnode, after, sec_sz, 0) != 0 ||
      after[510] != 0x55 || after[511] != 0xAA ||
      memcmp(before + 440, after + 440, 72) != 0 ||
      after[0] == 0) {
    if (err && cap > 0)
      snprintf(err, cap, "BIOS boot installation left MBR partition table in an unexpected state");
    goto out;
  }
  if (gpt) {
    unsigned char hdr[512] = {0};
    char gerr[256] = {0};
    uint64_t sec_chk = 0;
    if (pread_exact_nocache(bnode, hdr, sizeof hdr, (off_t)sec_sz) != 0 ||
        memcmp(hdr, "EFI PART", 8) != 0 ||
        gpt_bios_boot(bnode, sec_sz, &sec_chk, gerr, sizeof gerr) == 0) {
      if (err && cap > 0)
        snprintf(err, cap, "BIOS boot installation left GPT header in an unexpected state");
      goto out;
    }
  }

  rc = 0;
  if (log) {
    log(gpt ? "Legacy BIOS boot code installed (GRUB 2 in the BIOS boot partition; it starts bootmgr)."
            : "Legacy BIOS boot code installed (GRUB 2 in the MBR gap; it starts bootmgr).", lu);
  }

out:
  free(core_buf);
  rmtree_flat(work);
  return rc;
}

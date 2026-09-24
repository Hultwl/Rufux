#define _GNU_SOURCE
#include "create.h"
#include "device.h"
#include "writer.h"
#include "partition.h"
#include "mkfs_task.h"
#include "extract.h"
#include "boot.h"
#include "persist.h"
#include "badblocks.h"
#include "mount.h"
#include "priv.h"
#include "secureboot.h"
#include "exec.h"
#include "vhd.h"
#include "dosboot.h"
#include "wininstall.h"
#include "smart.h"
#include "vdisk.h"
#include "bootcheck.h"
#include "biosboot.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <strings.h>
#include <linux/fs.h>

void rufux_create_defaults(RufuxCreateOpts *o) {
  memset(o, 0, sizeof *o);
  o->mode = "dd";
  o->scheme = "gpt";
  o->fs = "vfat";
  o->label = "RUFUX";
  o->quick_format = 1;
  o->extended_label = 1;
  o->dry_run = 1;
}

// ---- Overall progress: every flow reports percent as prog(pct, 100).
// Sub-operations reporting byte progress are mapped into their stage
// window via ProgMap, so the bar moves continuously end to end.
typedef struct {
  RufuxCreateProgress prog;
  void *user;
  unsigned base; // stage start, percent
  unsigned span; // stage width, percent
} ProgMap;

static void mapped(unsigned long long done, unsigned long long total, void *u) {
  ProgMap *m = (ProgMap *)u;
  if (!m->prog) return;
  unsigned v = m->base;
  if (total) {
    unsigned long long add = m->span * done / total;
    if (add > m->span) add = m->span;
    v += (unsigned)add;
  }
  if (v > 100) v = 100;
  m->prog(v, 100, m->user);
}

static void stage(RufuxCreateProgress prog, void *user, unsigned pct) {
  if (prog && pct <= 100) prog(pct, 100, user);
}

static int is_dir(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_block(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISBLK(st.st_mode);
}

// Wait for a partition node after partprobe+settle.
static int wait_node(const char *p1, char *err, unsigned long cap) {
  int waited = 0;
  while (access(p1, F_OK) != 0 && waited < 100) { usleep(100000); waited++; }
  if (access(p1, F_OK) != 0) { snprintf(err, cap, "partition '%s' did not appear", p1); return -1; }
  return 0;
}

static void rescan_disk(const char *dst) {
  const char *pp[] = {"partprobe", dst, NULL};
  if (rufux_have("partprobe")) rufux_run(pp, 0);
  const char *us[] = {"udevadm", "settle", NULL};
  if (rufux_have("udevadm")) rufux_run(us, 0);
}

// Syslinux chainloader for BIOS (dos-scheme) flows. Best effort: without
// the binary there is no partition boot code, so say so loudly.
static int syslinux_best_effort(const char *part,
                                RufuxCreateLog log, void *luser,
                                char *err, unsigned long cap) {
  if (!rufux_have("syslinux")) {
    if (log) log("WARNING: syslinux not installed: no BIOS boot code written (install syslinux).", luser);
    return 0;
  }
  if (log) log("Installing Syslinux bootloader...", luser);
  return rufux_install_syslinux(part, 0, err, cap);
}

// Rufus dismounts the target's volumes before touching them instead of
// refusing: unmount every mounted partition belonging to this disk.
// Returns -1 if anything is still mounted afterwards.
static int unmount_disk(const char *dst, int dry_run,
                        RufuxCreateLog log, void *luser,
                        char *err, unsigned long cap) {
  const char *base = strrchr(dst, '/');
  base = base ? base + 1 : dst;
  FILE *f = fopen("/proc/mounts", "r");
  if (!f) return 0;
  char line[1024];
  char parts[32][128];
  int nparts = 0;
  size_t blen = strlen(base);
  while (fgets(line, sizeof line, f) && nparts < 32) {
    char dev[512] = {0};
    if (sscanf(line, "%511s", dev) != 1) continue;
    if (strncmp(dev, "/dev/", 5)) continue;
    const char *d = dev + 5;
    if (strncmp(d, base, blen)) continue;
    // dst itself, or dst + partition suffix (digits / p+digits)
    const char *rest = d + blen;
    if (rest[0] && rest[0] != 'p' && (rest[0] < '0' || rest[0] > '9')) continue;
    int dup = 0;
    for (int i = 0; i < nparts; i++)
      if (!strcmp(parts[i], dev)) { dup = 1; break; }
    if (!dup) snprintf(parts[nparts++], sizeof parts[0], "%s", dev);
  }
  fclose(f);
  for (int i = 0; i < nparts; i++) {
    char m[256];
    snprintf(m, sizeof m, "Unmounting %s...", parts[i]);
    if (log) log(m, luser);
    if (rufux_unmount(parts[i], dry_run, err, cap) != 0) {
      if (!dry_run) return -1;
    }
  }
  return 0;
}

static unsigned long long file_size(const char *p) {
  struct stat st;
  if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
  return (unsigned long long)st.st_size;
}

// FAT32 cannot hold files >= 4GiB: refuse early with a useful message
// (Rufus solves this with UEFI:NTFS; we point at NTFS/exFAT instead).
static int vfat_size_guard(const char *src, const char *fs, char *err, unsigned long cap) {
  if (!rufux_fs_is_fat(fs)) return 0;
  unsigned long long sz = file_size(src);
  if (sz > 0xFFFFFFFFULL) {
    snprintf(err, cap, "image is %.1f GiB: FAT cannot hold files >= 4 GiB; "
                       "use --fs ntfs or --fs exfat",
             sz / 1073741824.0);
    return -1;
  }
  return 0;
}

// Zero the first 16MB (Rufus "full format" equivalent for the boot area).
static int zero_head(const char *dst, RufuxCreateLog log, void *luser,
                     RufuxCreateProgress prog, void *puser,
                     char *err, unsigned long cap) {
  if (log) log("Clearing first 16MB (full format)...", luser);
  int fd = open(dst, O_WRONLY | O_CLOEXEC);
  if (fd < 0) { snprintf(err, cap, "cannot open '%s': %s", dst, strerror(errno)); return -1; }
  static char z[1 << 20];
  memset(z, 0, sizeof z);
  for (int i = 0; i < 16; i++) {
    size_t off = 0;
    while (off < sizeof z) {
      ssize_t w = write(fd, z + off, sizeof z - off);
      if (w < 0) {
        if (errno == EINTR) continue;
        snprintf(err, cap, "zero failed: %s", strerror(errno));
        close(fd);
        return -1;
      }
      off += (size_t)w;
    }
    if (prog) prog((unsigned long long)(i + 1), 16, puser);
  }
  if (fsync(fd) != 0) { snprintf(err, cap, "fsync failed"); close(fd); return -1; }
  close(fd);
  return 0;
}

// Erase the first and last MiB of a partition before formatting it.
// Rufus does the same (ClearPartition) and for the same reason: mkfs only
// overwrites the sectors its own superblock occupies, so the *backup* copies
// of whatever was there before survive (FAT32 keeps one at sector 6, NTFS one
// in the last sector of the volume). Leftovers make probers - and Windows -
// see a volume that no longer exists, which shows up as a 0-byte volume.
// A drive that cannot even take zeros is failing or write-protected, so stop.
static int wipe_part_edges(const char *part) {
  int fd = open(part, O_WRONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  unsigned long long sz = 0;
  if (ioctl(fd, BLKGETSIZE64, &sz) != 0) sz = 0;
  static char z[1 << 20];
  memset(z, 0, sizeof z);
  size_t head = (sz && sz < sizeof z) ? (size_t)sz : sizeof z;
  int bad = rufux_pwrite_all(fd, z, head, 0) != 0;
  if (!bad && sz > (unsigned long long)sizeof z * 2)
    bad = rufux_pwrite_all(fd, z, sizeof z, (off_t)(sz - sizeof z)) != 0;
  if (fsync(fd) != 0) bad = 1;
  close(fd);
  return bad ? -1 : 0;
}

// dd layout: badblocks 0-10, zero 10-15, write 15-(verify?85:100), verify 85-100.
static int run_badblocks(const char *dst, int passes, int allow_file,
                         unsigned base, unsigned span,
                         RufuxCreateProgress prog, void *puser,
                         RufuxCreateLog log, void *luser,
                         char *err, unsigned long cap) {
  for (int i = 1; i <= passes; i++) {
    char m[160];
    snprintf(m, sizeof m, "Checking device for bad blocks (destructive write patterns): pass %d/%d...",
             i, passes);
    if (log) log(m, luser);
    ProgMap pm = {prog, puser, base + span * (unsigned)(i - 1) / (unsigned)passes,
                  span / (unsigned)passes};
    unsigned long long bad = 0;
    // One pattern per gate pass, rotating 0xAA/0x55/0xFF/0x00.
    if (rufux_badblocks_write(dst, allow_file, 1, i - 1,
                              prog ? (RufuxScanProgress)mapped : NULL, &pm,
                              &bad, err, cap) != 0)
      return -1;
    snprintf(m, sizeof m, "Bad blocks pass %d/%d: %llu bad regions", i, passes, bad);
    if (log) log(m, luser);
    if (bad) { snprintf(err, cap, "%llu bad regions found", bad); return -1; }
  }
  return 0;
}

static int flow_dd(const char *src, const char *dst, const RufuxCreateOpts *o,
                   RufuxCreateProgress prog, void *puser,
                   RufuxCreateLog log, void *luser,
                   char *err, unsigned long cap) {
  ProgMap wm = {prog, puser, 15, o->verify ? 70 : 85};
  ProgMap vm = {prog, puser, 85, 15};
  RufuxWriteOpts w = {.dry_run = o->dry_run, .verify = o->verify,
                      .allow_fixed = o->allow_fixed, .allow_file = o->allow_file,
                      .yes = o->yes,
                      .vprog = prog ? (RufuxWriteProgress)mapped : NULL, .vuser = &vm};
  // VHDX, dynamic VHD, VMDK, QCOW2, VDI: expand onto the drive with qemu-img.
  RufuxVdisk vd;
  int vk = rufux_vdisk_detect(src, &vd, err, cap);
  if (vk < 0) return -1;
  if (vk > 0) {
    char vm[256];
    snprintf(vm, sizeof vm, "%s image detected (%.1f GiB expanded): converting with qemu-img.",
             vd.label, vd.virtual_size / 1073741824.0);
    if (log) log(vm, luser);
    int vrc = rufux_vdisk_write(src, &vd, dst, &w, prog ? (RufuxWriteProgress)mapped : NULL, &wm,
                                err, cap);
    if (vrc == 0) stage(prog, puser, 100);
    return vrc;
  }
  if (rufux_vhd_adjust(src, &w, log, luser, err, cap) != 0) return -1;
  int rc = rufux_write_image(src, dst, &w, prog ? (RufuxWriteProgress)mapped : NULL, &wm,
                             err, cap);
  if (rc == 0) stage(prog, puser, 100);
  return rc;
}

// dir layout: extract 0-90, autorun/persist 90-100.
static int flow_extract_dir(const char *src, const char *dir, const RufuxCreateOpts *o,
                            RufuxCreateProgress prog, void *puser,
                            RufuxCreateLog log, void *luser,
                            char *err, unsigned long cap) {
  if (vfat_size_guard(src, o->fs, err, cap) != 0) return -1;
  ProgMap em = {prog, puser, 0, 90};
  if (rufux_extract_iso_progress(src, dir, o->dry_run,
                                 prog ? (RufuxExtractProgress)mapped : NULL, &em,
                                 err, cap) != 0)
    return -1;
  if (o->extended_label) {
    if (log) log("Creating extended label and icon files (autorun.inf)...", luser);
    if (rufux_write_autorun(dir, o->label, o->dry_run, err, cap) != 0) return -1;
  }
  if (o->persist_mb && !o->dry_run) {
    if (rufux_create_persist(dir, "casper-rw", o->persist_mb, 0, err, cap) != 0) return -1;
  } else if (o->persist_mb) {
    char m[256];
    snprintf(m, sizeof m, "+ persist casper-rw %luMB in %s [dry-run]", o->persist_mb, dir);
    if (log) log(m, luser);
  }
  stage(prog, puser, 100);
  return 0;
}

// Whole-disk UEFI file flow with udisks2 auto-mount.
// Layout: partition 0-4, format 4-8, extract 8-82, persist 82-88,
// validate 88-90, unmount+MBR 90-100.
static int flow_extract_disk(const char *src, const char *dst, const RufuxCreateOpts *o,
                             RufuxCreateProgress prog, void *puser,
                             RufuxCreateLog log, void *luser,
                             char *err, unsigned long cap) {
  char p1[160], m[512];
  rufux_part1(dst, p1, sizeof p1);
  if (vfat_size_guard(src, o->fs, err, cap) != 0) return -1;
  if (o->dry_run && log) {
    // keep the classic step listing (also asserted by tests)
    snprintf(m, sizeof m, "steps:\n  1. partition %s %s/single (sfdisk)\n  2. format %s %s [%s]\n"
             "  3. mount %s (udisks2) + extract %s",
             dst, o->scheme, p1, o->fs, o->label ? o->label : "", p1, src);
    log(m, luser);
    if (o->persist_mb) {
      snprintf(m, sizeof m, "  4. persist casper-rw %luMB", o->persist_mb);
      log(m, luser);
    }
    snprintf(m, sizeof m, "  5. install-boot %s (syslinux MBR)", dst);
    log(m, luser);
    return 0;
  }

  RufuxPartOpts po = {.scheme = o->scheme, .layout = "single", .fs_main = o->fs, .dry_run = 0,
                      .allow_fixed = o->allow_fixed, .allow_file = 0, .yes = 1};
  if (rufux_partition(dst, &po, err, cap) != 0) return -1;
  stage(prog, puser, 4);
  rescan_disk(dst);
  if (wait_node(p1, err, cap) != 0) return -1;
  if (!o->quick_format) {
    ProgMap zm = {prog, puser, 4, 2};
    if (zero_head(p1, log, luser, prog ? (RufuxCreateProgress)mapped : NULL, &zm,
                  err, cap) != 0)
      return -1;
  }
  RufuxMkfsOpts mo = {.fs = o->fs, .label = o->label, .cluster_sectors = o->cluster_sectors,
                      .dry_run = 0, .allow_fixed = o->allow_fixed, .allow_file = 0, .yes = 1};
  snprintf(m, sizeof m, "Creating file system (%s)...", o->fs);
  if (log) log(m, luser);
  if (rufux_format(p1, &mo, err, cap) != 0) return -1;
  stage(prog, puser, 8);
  // BIOS flows need real boot code in the partition, not just an active
  // flag: chainload Syslinux (FAT/ext only; NTFS has its own PBR path).
  if (!strcmp(o->scheme, "dos") &&
      (rufux_fs_is_fat(o->fs) || !strncmp(o->fs, "ext", 3))) {
    if (syslinux_best_effort(p1, log, luser, err, cap) != 0) return -1;
  }
  char mnt[512] = {0};
  if (rufux_mount(p1, 0, mnt, sizeof mnt, err, cap) != 0) return -1;
  int rc = 0;
  ProgMap em = {prog, puser, 8, 74};
  if (rufux_extract_iso_progress(src, mnt, 0,
                                 prog ? (RufuxExtractProgress)mapped : NULL, &em,
                                 err, cap) != 0)
    rc = -1;
  if (!rc && o->extended_label) {
    if (log) log("Creating extended label and icon files (autorun.inf)...", luser);
    if (rufux_write_autorun(mnt, o->label, 0, err, cap) != 0) rc = -1;
  }
  if (!rc && o->persist_mb) {
    if (rufux_create_persist(mnt, "casper-rw", o->persist_mb, 0, err, cap) != 0) rc = -1;
  }
  stage(prog, puser, 88);
  if (!rc) rufux_bootcheck_tree(mnt, log, luser);  // advisory: revoked loaders are reported, not blocked
  if (!rc && o->uefi_validate) {
    char efi[768];
    snprintf(efi, sizeof efi, "%s/EFI/BOOT/bootx64.efi", mnt);
    if (access(efi, R_OK) == 0) {
      unsigned sub = 0;
      char verr[256] = {0};
      if (rufux_validate_efi(efi, &sub, verr, sizeof verr) == 0) {
        // Honest wording: this parses the PE header and checks the EFI
        // subsystem id only. No Authenticode/db/dbx signature is verified.
        if (log) log("UEFI media validation: EFI executable header OK (subsystem check only; no signature verified).", luser);
      } else {
        snprintf(m, sizeof m, "UEFI media validation warning: %s", verr);
        if (log) log(m, luser);
      }
    } else if (log) {
      log("UEFI media validation: no EFI/BOOT/bootx64.efi found, skipping.", luser);
    }
  }
  stage(prog, puser, 90);
  char uerr[512] = {0};
  if (rufux_unmount(p1, 0, uerr, sizeof uerr) != 0 && !rc) {
    snprintf(err, cap, "extracted but unmount failed: %s", uerr);
    rc = -1;
  }
  if (!rc) {
    RufuxBootOpts bo = {.kind = !strcmp(o->scheme, "gpt") ? "gpt" : "bios",
                        .dry_run = 0, .allow_fixed = o->allow_fixed, .yes = 1};
    if (rufux_install_mbr(dst, &bo, err, cap) != 0) rc = -1;
  }
  if (!rc) stage(prog, puser, 100);
  return rc;
}

// Non bootable: partition + format + MBR, no image.
// Layout: partition 0-30, format 30-85, MBR 85-100.
static int flow_format(const char *dst, const RufuxCreateOpts *o,
                       RufuxCreateProgress prog, void *puser,
                       RufuxCreateLog log, void *luser,
                       char *err, unsigned long cap) {
  char m[512];
  if (is_block(dst)) {
    snprintf(m, sizeof m, "Plan: partition %s %s/single, format %s, MBR (non bootable)",
             dst, o->scheme, o->fs);
    if (log) log(m, luser);
    if (o->dry_run) return 0;
    RufuxPartOpts po = {.scheme = o->scheme, .layout = "single", .fs_main = o->fs, .dry_run = 0,
                        .allow_fixed = o->allow_fixed, .allow_file = 0, .yes = 1};
    if (rufux_partition(dst, &po, err, cap) != 0) return -1;
    stage(prog, puser, 30);
    rescan_disk(dst);
    char p1[160];
    rufux_part1(dst, p1, sizeof p1);
    if (wait_node(p1, err, cap) != 0) return -1;
    if (!o->quick_format) {
      ProgMap zm = {prog, puser, 30, 10};
      if (zero_head(p1, log, luser, prog ? (RufuxCreateProgress)mapped : NULL, &zm,
                    err, cap) != 0)
        return -1;
    }
    RufuxMkfsOpts mo = {.fs = o->fs, .label = o->label, .cluster_sectors = o->cluster_sectors,
                        .dry_run = 0, .allow_fixed = o->allow_fixed, .allow_file = 0, .yes = 1};
    snprintf(m, sizeof m, "Creating file system (%s)...", o->fs);
    if (log) log(m, luser);
    if (rufux_format(p1, &mo, err, cap) != 0) return -1;
    stage(prog, puser, 85);
    if (!strcmp(o->scheme, "dos") &&
        (rufux_fs_is_fat(o->fs) || !strncmp(o->fs, "ext", 3))) {
      if (syslinux_best_effort(p1, log, luser, err, cap) != 0) return -1;
    }
    RufuxBootOpts bo = {.kind = !strcmp(o->scheme, "gpt") ? "gpt" : "bios",
                        .dry_run = 0, .allow_fixed = o->allow_fixed, .yes = 1};
    if (rufux_install_mbr(dst, &bo, err, cap) != 0) return -1;
    stage(prog, puser, 100);
    return 0;
  }
  // regular file: whole-file format (rootless)
  snprintf(m, sizeof m, "Plan: format file %s as %s (non bootable)", dst, o->fs);
  if (log) log(m, luser);
  if (o->dry_run) return 0;
  RufuxMkfsOpts mo = {.fs = o->fs, .label = o->label, .cluster_sectors = o->cluster_sectors,
                      .dry_run = 0, .allow_fixed = o->allow_fixed, .allow_file = 1, .yes = 1};
  if (rufux_format(dst, &mo, err, cap) != 0) return -1;
  stage(prog, puser, 100);
  return 0;
}

// FreeDOS bootable disk: DOS partition + FAT + system files (KERNEL.SYS
// first) + FreeDOS boot record + DOS MBR. src is unused ("none").
// Layout: partition 0-10, format 10-20, files 20-70, boot records 70-85,
// unmount 85-100.
static int flow_dos(const char *dst, const RufuxCreateOpts *o,
                    RufuxCreateProgress prog, void *puser,
                    RufuxCreateLog log, void *luser,
                    char *err, unsigned long cap) {
  if (log) log("Plan: DOS partition, FAT32, FreeDOS system files + boot record, DOS MBR", luser);
  if (o->dry_run) return 0;
  if (!is_block(dst)) {
    snprintf(err, cap, "dos mode needs a block device (got '%s')", dst);
    return -1;
  }
  const char *fd = rufux_freedos_dir();
  if (!fd) {
    snprintf(err, cap, "FreeDOS payload not found (needs res/freedos or /usr/share/rufux/freedos)");
    return -1;
  }
  RufuxPartOpts po = {.scheme = "dos", .layout = "single", .fs_main = "vfat", .dry_run = 0,
                      .allow_fixed = o->allow_fixed, .allow_file = 0, .yes = 1};
  if (rufux_partition(dst, &po, err, cap) != 0) return -1;
  stage(prog, puser, 10);
  rescan_disk(dst);
  char p1[160];
  rufux_part1(dst, p1, sizeof p1);
  if (wait_node(p1, err, cap) != 0) return -1;
  RufuxMkfsOpts mo = {.fs = "vfat", .label = o->label, .cluster_sectors = o->cluster_sectors,
                      .dry_run = 0, .allow_fixed = o->allow_fixed, .allow_file = 0, .yes = 1};
  if (rufux_format(p1, &mo, err, cap) != 0) return -1;
  stage(prog, puser, 20);
  char mnt[512] = {0};
  if (rufux_mount(p1, 0, mnt, sizeof mnt, err, cap) != 0) return -1;
  int rc = 0;
  if (rufux_dos_copy_files(fd, mnt, err, cap) != 0) rc = -1;
  stage(prog, puser, 70);
  char uerr[512] = {0};
  if (rufux_unmount(p1, 0, uerr, sizeof uerr) != 0 && !rc) {
    snprintf(err, cap, "files copied but unmount failed: %s", uerr);
    rc = -1;
  }
  if (!rc) {
    // Boot records go on after unmount (raw device access, mounts stay clean).
    char lab[12] = {0};
    snprintf(lab, sizeof lab, "%s", o->label ? o->label : "FREEDOS");
    if (rufux_dos_pbr_fd32(p1, 0, lab, err, cap) != 0) rc = -1;
    else if (rufux_dos_mbr(dst, err, cap) != 0) rc = -1;
  }
  stage(prog, puser, rc ? 70 : 100);
  return rc;
}

// Windows installation media, laid out like Rufus does it:
//   partition 1: NTFS, holds the extracted ISO (what Setup reads)
//   partition 2: 1 MiB UEFI:NTFS, raw image with the EFI loader + NTFS driver
// The data partition must come first and the small boot partition must not
// be typed as an ESP; see partition.c for why. scheme "dos" is the same
// layout on an MBR table (for UEFI firmwares that want MBR); it does not
// boot on legacy BIOS, see the note logged at the end of the flow.
// Layout: partition 0-5, UEFI:NTFS + format 5-10, extract 10-80,
// unattend 80-88, verify 88-90, unmount 90-100.
static int p2_of(const char *p1, char *p2, unsigned long cap) {
  size_t L = strlen(p1);
  if (L < 2 || p1[L - 1] != '1') return -1;
  snprintf(p2, cap, "%s", p1);
  p2[L - 1] = '2';
  return 0;
}

// Setup finds its media by looking for these files; a stick that lacks any
// of them boots fine and then dies with "a media driver is missing".
// Windows ISOs carry the same tree in both an ISO9660 and a UDF descriptor,
// and the two disagree on case, so every lookup below walks the tree
// comparing names case-insensitively rather than trusting one spelling.
// Returns the file size, or -1 when the path does not resolve.
static long long find_ci(const char *root, const char *relpath) {
  char cur[1152];
  snprintf(cur, sizeof cur, "%s", root);
  char rest[512];
  snprintf(rest, sizeof rest, "%s", relpath);
  char *save = NULL;
  for (char *seg = strtok_r(rest, "/", &save); seg; seg = strtok_r(NULL, "/", &save)) {
    DIR *d = opendir(cur);
    if (!d) return -1;
    struct dirent *e;
    char hit[256] = {0};
    while ((e = readdir(d)) != NULL) {
      if (!strcasecmp(e->d_name, seg)) { snprintf(hit, sizeof hit, "%s", e->d_name); break; }
    }
    closedir(d);
    if (!hit[0]) return -1;
    size_t L = strlen(cur);
    snprintf(cur + L, sizeof cur - L, "/%s", hit);
  }
  struct stat st;
  if (stat(cur, &st) != 0) return -1;
  return (long long)st.st_size;
}

static int verify_windows_tree(const char *root, RufuxCreateLog log, void *luser,
                               char *err, unsigned long cap) {
  char m[1280];
  // Setup finds its media by looking for these; a stick missing any of them
  // boots into WinPE and then dies with "a media driver is missing", because
  // what is actually missing is the installation source, not a driver.
  static const char *must[] = {
    "bootmgr", "setup.exe", "sources/setup.exe", "sources/boot.wim",
    "boot/bcd", "efi/microsoft/boot/bcd", NULL};
  for (int i = 0; must[i]; i++) {
    if (find_ci(root, must[i]) <= 0) {
      snprintf(err, cap, "extraction incomplete: '%s' is missing or empty on the stick "
                         "(Windows Setup reports this as a missing media driver)", must[i]);
      return -1;
    }
  }
  // The UEFI loader the firmware (or UEFI:NTFS) hands control to.
  static const char *loaders[] = {"efi/boot/bootx64.efi", "efi/boot/bootia32.efi",
                                  "efi/boot/bootaa64.efi", NULL};
  int have_loader = 0;
  for (int i = 0; loaders[i] && !have_loader; i++)
    if (find_ci(root, loaders[i]) > 0) have_loader = 1;
  if (!have_loader) {
    snprintf(err, cap, "extraction incomplete: no efi/boot/boot*.efi on the stick");
    return -1;
  }
  static const char *inst[] = {"sources/install.wim", "sources/install.esd", "sources/install.swm", NULL};
  for (int i = 0; inst[i]; i++) {
    long long sz = find_ci(root, inst[i]);
    if (sz > 0) {
      snprintf(m, sizeof m, "Verified %s (%.1f MiB) on the stick.", inst[i], sz / 1048576.0);
      if (log) log(m, luser);
      return 0;
    }
  }
  snprintf(err, cap, "extraction incomplete: sources/install.wim|esd|swm missing on the stick");
  return -1;
}

static int flow_windows(const char *src, const char *dst, const RufuxCreateOpts *o,
                        RufuxCreateProgress prog, void *puser,
                        RufuxCreateLog log, void *luser,
                        char *err, unsigned long cap) {
  char m[1024], p1[160], p2[160];
  int gpt = strcmp(o->scheme, "dos");
  // FAT32: one plain partition, no UEFI:NTFS driver, works with Secure Boot;
  // install.wim is split when it is over 4 GiB. NTFS: data partition + UEFI:NTFS.
  int fat = !strcmp(o->fs, "vfat") || !strcmp(o->fs, "fat32");
  const char *fsname = fat ? "vfat" : "ntfs";
  if (!fat && strcmp(o->fs, "ntfs")) {
    snprintf(err, cap, "Windows media supports FAT32 (vfat) or NTFS, not '%s'", o->fs);
    return -1;
  }
  rufux_part1(dst, p1, sizeof p1);
  if (o->dry_run && log) {
    snprintf(m, sizeof m,
             "steps:\n  1. partition %s %s: %s\n"
             "  2. %s\n"
             "  3. mount partition 1, extract %s%s\n"
             "  4. Windows customization (%s)\n  5. verify the copied Windows tree%s",
             dst, gpt ? "gpt" : "dos",
             fat ? "one FAT32 partition" : "NTFS data partition first, 1 MiB UEFI:NTFS last",
             fat ? "format partition 1 as FAT32" : "write UEFI:NTFS image to partition 2 (raw), format partition 1 as NTFS",
             src, fat ? " (install.wim split with wimlib if over 4 GiB)" : "",
             o->wue ? o->wue : "none",
             gpt ? "" : "\n  6. install legacy BIOS boot code (GRUB 2 in the MBR gap, starts bootmgr)");
    log(m, luser);
    return 0;
  }
  {
    RufuxWueArgs chk = {o->wue, o->locale, o->keyboard, o->timezone};
    if (rufux_windows_check(&chk, err, cap) != 0) return -1;  // before touching the disk
  }
  if (!fat && p2_of(p1, p2, sizeof p2) != 0) {
    snprintf(err, cap, "cannot derive the UEFI:NTFS partition from '%s'", p1);
    return -1;
  }
  RufuxPartOpts po = {.scheme = gpt ? "gpt" : "dos", .fs_main = fsname,
                      .layout = fat ? "single" : "main+uefintfs",
                      .dry_run = 0, .allow_fixed = o->allow_fixed, .allow_file = 0, .yes = 1};
  if (rufux_partition(dst, &po, err, cap) != 0) return -1;
  stage(prog, puser, 5);
  rescan_disk(dst);
  if (wait_node(p1, err, cap) != 0) return -1;
  if (!fat) {
    if (wait_node(p2, err, cap) != 0) return -1;
    if (log) log("Writing UEFI:NTFS boot partition...", luser);
    if (rufux_write_uefi_ntfs(p2, err, cap) != 0) return -1;
  }
  if (!o->quick_format) {  // "Quick format" unticked: clear the boot area first, as the other flows do
    ProgMap zm = {prog, puser, 5, 3};
    if (zero_head(p1, log, luser, prog ? (RufuxCreateProgress)mapped : NULL, &zm, err, cap) != 0)
      return -1;
  }
  if (wipe_part_edges(p1) != 0) {
    snprintf(err, cap, "cannot write to '%s': %s (write-protected or failing drive?)", p1, strerror(errno));
    return -1;
  }
  RufuxMkfsOpts main_o = {.fs = fsname, .label = o->label, .cluster_sectors = o->cluster_sectors,
                          .dry_run = 0, .allow_fixed = o->allow_fixed, .allow_file = 0, .yes = 1};
  snprintf(m, sizeof m, "Creating file system (%s)...", fsname);
  if (log) log(m, luser);
  if (rufux_format(p1, &main_o, err, cap) != 0) return -1;
  // Fresh signatures settle asynchronously in udev/udisks; mounting at once
  // can see stale data, so rescan and retry the mount once.
  rescan_disk(dst);
  stage(prog, puser, 10);
  char mnt[512] = {0};
  if (rufux_mount(p1, 0, mnt, sizeof mnt, err, cap) != 0) {
    rescan_disk(dst);
    if (rufux_mount(p1, 0, mnt, sizeof mnt, err, cap) != 0) return -1;
  }
  int rc = 0;
  ProgMap em = {prog, puser, 10, 70};
  if (fat || o->split_wim_mb) {
    if (rufux_extract_windows_split(src, mnt, o->split_wim_mb ? o->split_wim_mb : 3800,
                                    o->split_wim_mb != 0,
                                    prog ? (RufuxExtractProgress)mapped : NULL, &em,
                                    log, luser, err, cap) != 0)
      rc = -1;
  } else if (rufux_extract_iso_progress(src, mnt, 0,
                                        prog ? (RufuxExtractProgress)mapped : NULL, &em,
                                        err, cap) != 0)
    rc = -1;
  if (!rc && (o->wue && strcmp(o->wue, "none"))) {
    RufuxWueArgs wa = {o->wue, o->locale, o->keyboard, o->timezone};
    if (rufux_windows_customize(mnt, &wa, log, luser, err, cap) != 0) rc = -1;
  }
  stage(prog, puser, 88);
  if (!rc) rc = verify_windows_tree(mnt, log, luser, err, cap);
  if (!rc) rufux_bootcheck_tree(mnt, log, luser);  // advisory
  if (!rc && o->uefi_validate) {
    char efi[768];
    snprintf(efi, sizeof efi, "%s/efi/boot/bootx64.efi", mnt);
    if (access(efi, R_OK) == 0) {
      unsigned sub = 0;
      char verr[256] = {0};
      if (rufux_validate_efi(efi, &sub, verr, sizeof verr) == 0) {
        if (log) log("UEFI media validation: Windows bootloader header OK (subsystem check only).", luser);
      } else if (log) {
        snprintf(m, sizeof m, "UEFI media validation warning: %s", verr);
        log(m, luser);
      }
    }
  }
  stage(prog, puser, 90);
  int bios_ok = 0;
  if (!rc && !gpt) {
    // MBR drive: make it boot on legacy BIOS machines too. Not fatal: without it the
    // drive is still a complete UEFI installer, and the log says so.
    char be[512] = {0};
    if (log) log("Installing legacy BIOS boot code...", luser);
    if (rufux_windows_bios_install(dst, mnt, log, luser, be, sizeof be) == 0) bios_ok = 1;
    else if (log) {
      snprintf(m, sizeof m, "WARNING: no legacy BIOS boot code: %s. The drive boots on UEFI machines only.", be);
      log(m, luser);
    }
  }
  sync();
  char uerr[512] = {0};
  if (rufux_unmount(p1, 0, uerr, sizeof uerr) != 0 && !rc) {
    snprintf(err, cap, "installed but unmount failed: %s", uerr);
    rc = -1;
  }
  // ntfs-3g raises the volume dirty flag while the volume is mounted. A clean
  // unmount clears it, but a racing partprobe/udev rescan can leave it set,
  // and Windows then treats the volume as needing a repair pass rather than
  // as installation media. Clearing it is cheap and safe.
  if (!rc && !fat && rufux_have("ntfsfix")) {
    const char *nf[] = {"ntfsfix", "-d", p1, NULL};
    if (rufux_run(nf, 0) != 0 && log)
      log("WARNING: could not clear the NTFS dirty flag (ntfsfix failed).", luser);
  }
  if (!rc && gpt && log)
    log("NOTE: this GPT drive boots on UEFI machines only (choose the MBR scheme for legacy BIOS boot too).", luser);
  if (!rc && !gpt && log)
    log(bios_ok ? "This drive boots on both UEFI and legacy BIOS machines."
                : "NOTE: this drive boots on UEFI machines only (no legacy BIOS boot code was installed).", luser);
  if (!rc) stage(prog, puser, 100);
  return rc;
}

int rufux_create(const char *src, const char *dst, const RufuxCreateOpts *o,
                 RufuxCreateProgress prog, void *puser,
                 RufuxCreateLog log, void *luser,
                 char *err, unsigned long errcap) {
  char m[1024];
  snprintf(m, sizeof m, "Rufux: %s -> %s [mode=%s scheme=%s fs=%s label=%s]%s",
           src ? src : "(none)", dst, o->mode, o->scheme, o->fs,
           o->label ? o->label : "", o->dry_run ? " [dry-run]" : "");
  if (log) log(m, luser);

  if (!o->dry_run && !o->yes) {
    snprintf(err, errcap, "refusing real run without --yes (use --dry-run to plan)");
    return -1;
  }
  if (!o->dry_run && is_block(dst)) {
    if (rufux_need_root_for_block(dst, err, errcap) != 0) return -1;
    // Dismount our own target first (consent was the START warning);
    // the per-operation checks below re-verify it afterwards.
    if (unmount_disk(dst, 0, log, luser, err, errcap) != 0) {
      if (!err[0]) snprintf(err, errcap, "cannot unmount %s (close files and retry)", dst);
      return -1;
    }
  }
  // Drive health first: a failing drive is a bad place to put an installer.
  if (is_block(dst)) {
    if (o->dry_run) {
      if (log) log("+ SMART health check (smartctl) [dry-run]", luser);
    } else {
      char sm[256];
      RufuxSmartState ss = rufux_smart_check(dst, sm, sizeof sm);
      if (log) log(sm, luser);
      if (ss == RUFUX_SMART_FAILED && !o->ignore_smart) {
        snprintf(err, errcap, "%s. Refusing to write; use --ignore-smart to override", sm);
        return -1;
      }
    }
  }
  // dd layout reserves 0-15 for checks; extract/format start at 0.
  if (o->badblock_passes > 0 && !o->dry_run) {
    unsigned span = !strcmp(o->mode, "dd") ? 10 : 8;
    if (run_badblocks(dst, o->badblock_passes, o->allow_file, 0, span,
                      prog, puser, log, luser, err, errcap) != 0)
      return -1;
  } else if (o->badblock_passes > 0 && log) {
    snprintf(m, sizeof m, "+ bad-blocks check %d pass(es) on %s [dry-run]", o->badblock_passes, dst);
    log(m, luser);
  }
  if (!o->dry_run && !o->quick_format && is_block(dst) && !strcmp(o->mode, "dd")) {
    ProgMap zm = {prog, puser, 10, 5};
    if (zero_head(dst, log, luser, prog ? (RufuxCreateProgress)mapped : NULL, &zm,
                  err, errcap) != 0)
      return -1;
  }

  int rc;
  if (!strcmp(o->mode, "dd")) {
    rc = flow_dd(src, dst, o, prog, puser, log, luser, err, errcap);
  } else if (!strcmp(o->mode, "extract")) {
    if (is_dir(dst)) rc = flow_extract_dir(src, dst, o, prog, puser, log, luser, err, errcap);
    else rc = flow_extract_disk(src, dst, o, prog, puser, log, luser, err, errcap);
  } else if (!strcmp(o->mode, "format")) {
    rc = flow_format(dst, o, prog, puser, log, luser, err, errcap);
  } else if (!strcmp(o->mode, "dos")) {
    rc = flow_dos(dst, o, prog, puser, log, luser, err, errcap);
  } else if (!strcmp(o->mode, "windows")) {
    if (!src) { snprintf(err, errcap, "windows mode needs an image"); return -1; }
    rc = flow_windows(src, dst, o, prog, puser, log, luser, err, errcap);
  } else {
    snprintf(err, errcap, "unknown mode '%s' (dd|extract|format|dos|windows)", o->mode);
    return -1;
  }
  if (rc == 0 && log) {
    snprintf(m, sizeof m, "%s", o->dry_run ? "Plan OK (dry-run, nothing written)." : "Done.");
    log(m, luser);
  }
  return rc;
}

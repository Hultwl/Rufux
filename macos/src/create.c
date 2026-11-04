#include "create.h"
#include "badblocks.h"
#include "biosboot.h"
#include "boot.h"
#include "bootcheck.h"
#include "device.h"
#include "dosboot.h"
#include "exec.h"
#include "extract.h"
#include "mkfs_task.h"
#include "mount.h"
#include "ntfs_populate.h"
#include "partition.h"
#include "persist.h"
#include "platform_macos.h"
#include "priv.h"
#include "secureboot.h"
#include "smart.h"
#include "vdisk.h"
#include "vhd.h"
#include "wininstall.h"
#include "writer.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

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

typedef struct {
  RufuxCreateProgress prog;
  void *user;
  unsigned base;
  unsigned span;
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
  return p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_disk_dev(const char *p) {
  struct stat st;
  return p && stat(p, &st) == 0 && (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode));
}

static int is_reg_file(const char *p) {
  struct stat st;
  return p && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static unsigned long long file_size(const char *p) {
  struct stat st;
  if (!p || stat(p, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
  return (unsigned long long)st.st_size;
}

static void rm_rf_dir(const char *dir) {
  if (!dir || !dir[0] || strcmp(dir, "/") == 0) return;
  const char *rm[] = {"/bin/rm", "-rf", dir, NULL};
  (void)rufux_run(rm, 0);
}

static int wait_node(const char *slice_node, char *err, unsigned long cap) {
  for (int i = 0; i < 100; i++) {
    if (access(slice_node, F_OK) == 0) return 0;
    usleep(50000); // up to 5.0s
  }
  if (access(slice_node, F_OK) != 0) {
    if (err && cap > 0) snprintf(err, cap, "partition '%s' did not appear", slice_node);
    return -1;
  }
  return 0;
}

// Partitions `dst` (either a real /dev/[r]diskN device or an --allow-file regular image).
// When `dst` is a regular file, attaches it via hdiutil CRawDiskImage -nomount so slice
// nodes /dev/diskKs1, /dev/diskKs2, ... exist for formatting and mounting.
static int partition_and_attach_if_file(const char *dst, const RufuxPartOpts *po_in,
                                        char *work_disk, size_t wcap,
                                        char *attached_disk, size_t acap,
                                        int *sub_allow_fixed,
                                        char *err, unsigned long errcap) {
  if (attached_disk && acap > 0) attached_disk[0] = 0;
  if (work_disk && wcap > 0) work_disk[0] = 0;

  if (rufux_check_target(dst, po_in->allow_fixed, po_in->allow_file, err, errcap) != 0)
    return -1;

  if (rufux_partition(dst, po_in, err, errcap) != 0)
    return -1;

  if (is_reg_file(dst)) {
    if (rufux_macos_attach_raw_image(dst, 0, attached_disk, acap) != 0) {
      if (err && errcap > 0)
        snprintf(err, errcap, "failed to attach partitioned image '%s' via hdiutil", dst);
      return -1;
    }
    snprintf(work_disk, wcap, "%s", attached_disk);
    if (sub_allow_fixed) *sub_allow_fixed = 1;

    char s1[160];
    rufux_macos_part_slice(work_disk, 1, s1, sizeof s1);
    for (int i = 0; i < 60 && access(s1, F_OK) != 0; i++) usleep(50000);
    if (access(s1, F_OK) != 0) {
      // Detach and re-attach once so IOMediaBSDClient rescans fresh partition table.
      (void)rufux_macos_detach_disk(attached_disk);
      attached_disk[0] = 0;
      if (rufux_macos_attach_raw_image(dst, 0, attached_disk, acap) != 0) {
        if (err && errcap > 0)
          snprintf(err, errcap, "failed to re-attach partitioned image '%s'", dst);
        return -1;
      }
      snprintf(work_disk, wcap, "%s", attached_disk);
    }
  } else {
    rufux_macos_block_node(dst, work_disk, wcap);
    if (sub_allow_fixed) *sub_allow_fixed = po_in->allow_fixed;
  }
  return 0;
}

static int mount_native_private(const char *part_dev, int touch_no_index,
                                char *mnt_out, size_t mcap,
                                int *used_rufux_mount,
                                char *err, unsigned long errcap) {
  if (mnt_out && mcap > 0) mnt_out[0] = 0;
  if (used_rufux_mount) *used_rufux_mount = 0;

  char bnode[PATH_MAX];
  rufux_macos_block_node(part_dev, bnode, sizeof bnode);

  // Unmount any auto-mount first so we get a clean private nobrowse mountpoint.
  {
    char uout[256];
    const char *uav[] = {"/usr/sbin/diskutil", "unmount", "force", bnode, NULL};
    (void)rufux_capture(uav, uout, sizeof uout);
  }

  char tmpl[PATH_MAX];
  const char *tbase = getenv("RUFUX_TMPDIR");
  if (!tbase || !tbase[0]) tbase = "/tmp";
  snprintf(tmpl, sizeof tmpl, "%s/rufux-privmnt-XXXXXX", tbase);
  if (mkdtemp(tmpl)) {
    char dout[512] = {0};
    const char *dav[] = {
      "/usr/sbin/diskutil", "mount", "nobrowse", "-mountPoint", tmpl, bnode, NULL
    };
    if (rufux_capture(dav, dout, sizeof dout) == 0) {
      if (touch_no_index) {
        char ni[PATH_MAX * 2];
        snprintf(ni, sizeof ni, "%s/.metadata_never_index", tmpl);
        int fd = open(ni, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        if (fd >= 0) close(fd);
      }
      snprintf(mnt_out, mcap, "%s", tmpl);
      return 0;
    }
    (void)rmdir(tmpl);
  }

  // Fallback to rufux_mount (e.g. if custom mountPoint was declined).
  if (rufux_mount(bnode, 0, mnt_out, (unsigned long)mcap, err, errcap) != 0)
    return -1;
  if (used_rufux_mount) *used_rufux_mount = 1;
  if (touch_no_index && mnt_out[0]) {
    char ni[PATH_MAX * 2];
    snprintf(ni, sizeof ni, "%s/.metadata_never_index", mnt_out);
    int fd = open(ni, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd >= 0) close(fd);
  }
  return 0;
}

static int unmount_native_private(const char *part_dev, const char *mnt_dir,
                                  int used_rufux_mount,
                                  char *err, unsigned long errcap) {
  sync();
  if (used_rufux_mount) {
    return rufux_unmount(part_dev, 0, err, errcap);
  }
  char bnode[PATH_MAX], dout[512] = {0};
  rufux_macos_block_node(part_dev, bnode, sizeof bnode);
  const char *target = (mnt_dir && mnt_dir[0]) ? mnt_dir : bnode;
  const char *dav1[] = {"/usr/sbin/diskutil", "unmount", target, NULL};
  if (rufux_capture(dav1, dout, sizeof dout) != 0) {
    usleep(100000);
    const char *dav2[] = {"/usr/sbin/diskutil", "unmount", "force", target, NULL};
    if (rufux_capture(dav2, dout, sizeof dout) != 0) {
      const char *uav[] = {"/sbin/umount", "-f", target, NULL};
      if (rufux_capture(uav, dout, sizeof dout) != 0) {
        if (err && errcap > 0)
          snprintf(err, errcap, "unmount '%s' failed: %s", target, dout);
        return -1;
      }
    }
  }
  if (mnt_dir && mnt_dir[0]) (void)rmdir(mnt_dir);
  return 0;
}

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

static int zero_head(const char *dst, RufuxCreateLog log, void *luser,
                     RufuxCreateProgress prog, void *puser,
                     char *err, unsigned long cap) {
  if (log) log("Clearing first 16MB (full format)...", luser);
  char bnode[PATH_MAX];
  rufux_macos_block_node(dst, bnode, sizeof bnode);
  int fd = open(bnode, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    snprintf(err, cap, "cannot open '%s': %s", bnode, strerror(errno));
    return -1;
  }
  unsigned char *z = calloc(1, 1u << 20);
  if (!z) {
    close(fd);
    snprintf(err, cap, "out of memory");
    return -1;
  }
  unsigned long long max_sz = rufux_macos_dev_bytes(bnode);
  for (int i = 0; i < 16; i++) {
    unsigned long long off = (unsigned long long)i * (1ULL << 20);
    if (max_sz && off >= max_sz) break;
    size_t chunk = (1u << 20);
    if (max_sz && off + chunk > max_sz) chunk = (size_t)(max_sz - off);
    if (rufux_pwrite_all(fd, z, chunk, (off_t)off) != 0) {
      snprintf(err, cap, "zero failed: %s", strerror(errno));
      free(z);
      close(fd);
      return -1;
    }
    if (prog) prog((unsigned long long)(i + 1), 16, puser);
  }
  free(z);
  if (fsync(fd) != 0) {
    snprintf(err, cap, "fsync failed");
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

static int wipe_part_edges(const char *part) {
  char bnode[PATH_MAX];
  rufux_macos_block_node(part, bnode, sizeof bnode);
  int fd = open(bnode, O_WRONLY | O_CLOEXEC);
  if (fd < 0 && errno == EBUSY) {
    char uout[256];
    const char *uav[] = {"/usr/sbin/diskutil", "unmount", "force", bnode, NULL};
    (void)rufux_capture(uav, uout, sizeof uout);
    usleep(50000);
    fd = open(bnode, O_WRONLY | O_CLOEXEC);
  }
  if (fd < 0) return -1;
  unsigned long long sz = rufux_macos_dev_bytes(bnode);
  unsigned char *z = calloc(1, 1u << 20);
  if (!z) { close(fd); return -1; }
  size_t head = (sz && sz < (1ULL << 20)) ? (size_t)sz : (1u << 20);
  int bad = rufux_pwrite_all(fd, z, head, 0) != 0;
  if (!bad && sz > (2ULL << 20))
    bad = rufux_pwrite_all(fd, z, 1u << 20, (off_t)(sz - (1ULL << 20))) != 0;
  if (fsync(fd) != 0) bad = 1;
  free(z);
  close(fd);
  return bad ? -1 : 0;
}

static int resolve_ci_path(const char *root, const char *relpath,
                           char *out, size_t cap, long long *sz_out) {
  char cur[PATH_MAX];
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
  struct stat st;
  if (stat(cur, &st) != 0) return -1;
  if (out && cap > 0) snprintf(out, cap, "%s", cur);
  if (sz_out) *sz_out = (long long)st.st_size;
  return 0;
}

static void uefi_validate_in_tree(const char *root, int is_win,
                                  RufuxCreateLog log, void *luser) {
  char efi[PATH_MAX], m[512];
  if (resolve_ci_path(root, "efi/boot/bootx64.efi", efi, sizeof efi, NULL) == 0 &&
      access(efi, R_OK) == 0) {
    unsigned sub = 0;
    char verr[256] = {0};
    if (rufux_validate_efi(efi, &sub, verr, sizeof verr) == 0) {
      if (log) {
        log(is_win ? "UEFI media validation: Windows bootloader header OK (subsystem check only)."
                   : "UEFI media validation: EFI executable header OK (subsystem check only; no signature verified).",
            luser);
      }
    } else if (log) {
      snprintf(m, sizeof m, "UEFI media validation warning: %s", verr);
      log(m, luser);
    }
  } else if (!is_win && log) {
    log("UEFI media validation: no EFI/BOOT/bootx64.efi found, skipping.", luser);
  }
}

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
    if (rufux_badblocks_write(dst, allow_file, 1, i - 1,
                              prog ? (RufuxScanProgress)mapped : NULL, &pm,
                              &bad, err, cap) != 0)
      return -1;
    snprintf(m, sizeof m, "Bad blocks pass %d/%d: %llu bad regions", i, passes, bad);
    if (log) log(m, luser);
    if (bad) {
      snprintf(err, cap, "%llu bad regions found", bad);
      return -1;
    }
  }
  return 0;
}

static int flow_dd(const char *src, const char *dst, const RufuxCreateOpts *o,
                   RufuxCreateProgress prog, void *puser,
                   RufuxCreateLog log, void *luser,
                   char *err, unsigned long cap) {
  ProgMap wm = {prog, puser, 15, o->verify ? 70 : 85};
  ProgMap vm = {prog, puser, 85, 15};
  RufuxWriteOpts w = {
    .dry_run = o->dry_run, .verify = o->verify,
    .allow_fixed = o->allow_fixed, .allow_file = o->allow_file,
    .yes = o->yes,
    .vprog = prog ? (RufuxWriteProgress)mapped : NULL, .vuser = &vm
  };
  RufuxVdisk vd;
  int vk = rufux_vdisk_detect(src, &vd, err, cap);
  if (vk < 0) return -1;
  if (vk > 0) {
    char msg[256];
    snprintf(msg, sizeof msg, "%s image detected (%.1f GiB expanded): converting with qemu-img.",
             vd.label, vd.virtual_size / 1073741824.0);
    if (log) log(msg, luser);
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

static int flow_extract_disk(const char *src, const char *dst, const RufuxCreateOpts *o,
                             RufuxCreateProgress prog, void *puser,
                             RufuxCreateLog log, void *luser,
                             char *err, unsigned long cap) {
  char p1_plan[160], m[512];
  rufux_part1(dst, p1_plan, sizeof p1_plan);
  if (vfat_size_guard(src, o->fs, err, cap) != 0) return -1;
  if (o->dry_run) {
    if (rufux_check_target(dst, o->allow_fixed, o->allow_file, err, cap) != 0) return -1;
    if (log) {
      snprintf(m, sizeof m, "steps:\n  1. partition %s %s/single\n  2. format %s %s [%s]\n"
               "  3. populate %s + extract %s",
               dst, o->scheme, p1_plan, o->fs, o->label ? o->label : "", p1_plan, src);
      log(m, luser);
      if (o->persist_mb) {
        snprintf(m, sizeof m, "  4. persist casper-rw %luMB", o->persist_mb);
        log(m, luser);
      }
      snprintf(m, sizeof m, "  5. install-boot %s (syslinux MBR)", dst);
      log(m, luser);
    }
    return 0;
  }

  int rc = -1;
  char work_disk[160] = {0}, attached_disk[160] = {0}, p1[160] = {0};
  char mnt[PATH_MAX] = {0}, stg[PATH_MAX] = {0};
  int is_mounted = 0, used_rmount = 0, sub_allow_fixed = o->allow_fixed;

  RufuxPartOpts po = {
    .scheme = o->scheme, .layout = "single", .fs_main = o->fs, .dry_run = 0,
    .allow_fixed = o->allow_fixed, .allow_file = o->allow_file, .yes = 1
  };
  if (partition_and_attach_if_file(dst, &po, work_disk, sizeof work_disk,
                                   attached_disk, sizeof attached_disk,
                                   &sub_allow_fixed, err, cap) != 0)
    goto out;

  stage(prog, puser, 4);
  rufux_macos_part_slice(work_disk, 1, p1, sizeof p1);
  if (wait_node(p1, err, cap) != 0) goto out;

  if (!o->quick_format) {
    ProgMap zm = {prog, puser, 4, 2};
    if (zero_head(p1, log, luser, prog ? (RufuxCreateProgress)mapped : NULL, &zm, err, cap) != 0)
      goto out;
  }
  (void)wipe_part_edges(p1);

  RufuxMkfsOpts mo = {
    .fs = o->fs, .label = o->label, .cluster_sectors = o->cluster_sectors,
    .dry_run = 0, .allow_fixed = sub_allow_fixed, .allow_file = 0, .yes = 1
  };

  int is_ext = (strncmp(o->fs, "ext", 3) == 0);
  int is_ntfs = (strcmp(o->fs, "ntfs") == 0);

  if (is_ext || is_ntfs) {
    unsigned long long need = file_size(src) + ((unsigned long long)o->persist_mb << 20) + (64ULL << 20);
    if (rufux_scratch_dir(need, src, stg, sizeof stg) != 0) {
      snprintf(err, cap, "insufficient scratch space to stage '%s' (set RUFUX_TMPDIR)", src);
      goto out;
    }
    if (is_ntfs) {
      snprintf(m, sizeof m, "Creating file system (%s)...", o->fs);
      if (log) log(m, luser);
      if (rufux_format(p1, &mo, err, cap) != 0) goto out;
    }
    stage(prog, puser, 8);

    ProgMap em = {prog, puser, 8, 52};
    if (rufux_extract_iso_progress(src, stg, 0,
                                   prog ? (RufuxExtractProgress)mapped : NULL, &em,
                                   err, cap) != 0)
      goto out;
    if (o->extended_label) {
      if (log) log("Creating extended label and icon files (autorun.inf)...", luser);
      if (rufux_write_autorun(stg, o->label, 0, err, cap) != 0) goto out;
    }
    if (o->persist_mb) {
      if (rufux_create_persist(stg, "casper-rw", o->persist_mb, 0, err, cap) != 0) goto out;
    }
    rufux_bootcheck_tree(stg, log, luser);
    if (o->uefi_validate) uefi_validate_in_tree(stg, 0, log, luser);

    if (is_ext) {
      snprintf(m, sizeof m, "Creating and populating file system (%s)...", o->fs);
      if (log) log(m, luser);
      if (rufux_format_ext_from_dir(p1, &mo, stg, err, cap) != 0) goto out;
      if (!strcmp(o->scheme, "dos")) {
        if (syslinux_best_effort(p1, log, luser, err, cap) != 0) goto out;
      }
    } else {
      ProgMap nm = {prog, puser, 60, 28};
      if (rufux_ntfs_populate_dir(p1, stg, prog ? (RufuxNtfsProgress)mapped : NULL, &nm,
                                  err, cap) != 0)
        goto out;
    }
    stage(prog, puser, 90);
  } else {
    snprintf(m, sizeof m, "Creating file system (%s)...", o->fs);
    if (log) log(m, luser);
    if (rufux_format(p1, &mo, err, cap) != 0) goto out;
    stage(prog, puser, 8);

    if (!strcmp(o->scheme, "dos") && rufux_fs_is_fat(o->fs)) {
      if (syslinux_best_effort(p1, log, luser, err, cap) != 0) goto out;
    }

    if (mount_native_private(p1, 1, mnt, sizeof mnt, &used_rmount, err, cap) != 0) goto out;
    is_mounted = 1;

    ProgMap em = {prog, puser, 8, 74};
    if (rufux_extract_iso_progress(src, mnt, 0,
                                   prog ? (RufuxExtractProgress)mapped : NULL, &em,
                                   err, cap) != 0)
      goto out;
    if (o->extended_label) {
      if (log) log("Creating extended label and icon files (autorun.inf)...", luser);
      if (rufux_write_autorun(mnt, o->label, 0, err, cap) != 0) goto out;
    }
    if (o->persist_mb) {
      if (rufux_create_persist(mnt, "casper-rw", o->persist_mb, 0, err, cap) != 0) goto out;
    }
    stage(prog, puser, 88);
    rufux_bootcheck_tree(mnt, log, luser);
    if (o->uefi_validate) uefi_validate_in_tree(mnt, 0, log, luser);
    stage(prog, puser, 90);

    char uerr[512] = {0};
    is_mounted = 0;
    if (unmount_native_private(p1, mnt, used_rmount, uerr, sizeof uerr) != 0) {
      snprintf(err, cap, "extracted but unmount failed: %s", uerr);
      goto out;
    }
  }

  {
    RufuxBootOpts bo = {
      .kind = !strcmp(o->scheme, "gpt") ? "gpt" : "bios",
      .dry_run = 0, .allow_fixed = sub_allow_fixed, .allow_file = 0, .yes = 1
    };
    if (rufux_install_mbr(work_disk, &bo, err, cap) != 0) goto out;
  }

  stage(prog, puser, 100);
  rc = 0;

out:
  if (is_mounted) {
    char uerr[256];
    (void)unmount_native_private(p1, mnt, used_rmount, uerr, sizeof uerr);
  }
  if (stg[0]) rm_rf_dir(stg);
  if (attached_disk[0]) {
    char uerr[256];
    (void)rufux_macos_unmount_disk(attached_disk, uerr, sizeof uerr);
    (void)rufux_macos_detach_disk(attached_disk);
  }
  return rc;
}

static int flow_format(const char *dst, const RufuxCreateOpts *o,
                       RufuxCreateProgress prog, void *puser,
                       RufuxCreateLog log, void *luser,
                       char *err, unsigned long cap) {
  char m[512];
  if (is_disk_dev(dst)) {
    snprintf(m, sizeof m, "Plan: partition %s %s/single, format %s, MBR (non bootable)",
             dst, o->scheme, o->fs);
    if (log) log(m, luser);
    if (o->dry_run) {
      return rufux_check_target(dst, o->allow_fixed, o->allow_file, err, cap);
    }
    char bnode[PATH_MAX], p1[160];
    rufux_macos_block_node(dst, bnode, sizeof bnode);
    RufuxPartOpts po = {
      .scheme = o->scheme, .layout = "single", .fs_main = o->fs, .dry_run = 0,
      .allow_fixed = o->allow_fixed, .allow_file = 0, .yes = 1
    };
    if (rufux_partition(bnode, &po, err, cap) != 0) return -1;
    stage(prog, puser, 30);
    rufux_macos_part_slice(bnode, 1, p1, sizeof p1);
    if (wait_node(p1, err, cap) != 0) return -1;
    if (!o->quick_format) {
      ProgMap zm = {prog, puser, 30, 10};
      if (zero_head(p1, log, luser, prog ? (RufuxCreateProgress)mapped : NULL, &zm, err, cap) != 0)
        return -1;
    }
    (void)wipe_part_edges(p1);
    RufuxMkfsOpts mo = {
      .fs = o->fs, .label = o->label, .cluster_sectors = o->cluster_sectors,
      .dry_run = 0, .allow_fixed = o->allow_fixed, .allow_file = 0, .yes = 1
    };
    snprintf(m, sizeof m, "Creating file system (%s)...", o->fs);
    if (log) log(m, luser);
    if (rufux_format(p1, &mo, err, cap) != 0) return -1;
    stage(prog, puser, 85);
    if (!strcmp(o->scheme, "dos") &&
        (rufux_fs_is_fat(o->fs) || !strncmp(o->fs, "ext", 3))) {
      if (syslinux_best_effort(p1, log, luser, err, cap) != 0) return -1;
    }
    RufuxBootOpts bo = {
      .kind = !strcmp(o->scheme, "gpt") ? "gpt" : "bios",
      .dry_run = 0, .allow_fixed = o->allow_fixed, .allow_file = 0, .yes = 1
    };
    if (rufux_install_mbr(bnode, &bo, err, cap) != 0) return -1;
    stage(prog, puser, 100);
    return 0;
  }

  snprintf(m, sizeof m, "Plan: format file %s as %s (non bootable)", dst, o->fs);
  if (log) log(m, luser);
  if (o->dry_run) {
    return rufux_check_target(dst, o->allow_fixed, o->allow_file, err, cap);
  }
  RufuxMkfsOpts mo = {
    .fs = o->fs, .label = o->label, .cluster_sectors = o->cluster_sectors,
    .dry_run = 0, .allow_fixed = o->allow_fixed, .allow_file = o->allow_file, .yes = 1
  };
  if (rufux_format(dst, &mo, err, cap) != 0) return -1;
  stage(prog, puser, 100);
  return 0;
}

static int flow_dos(const char *dst, const RufuxCreateOpts *o,
                    RufuxCreateProgress prog, void *puser,
                    RufuxCreateLog log, void *luser,
                    char *err, unsigned long cap) {
  if (log) log("Plan: DOS partition, FAT32, FreeDOS system files + boot record, DOS MBR", luser);
  if (o->dry_run) {
    return rufux_check_target(dst, o->allow_fixed, o->allow_file, err, cap);
  }
  const char *fd_dir = rufux_freedos_dir();
  if (!fd_dir) {
    snprintf(err, cap, "FreeDOS payload not found (needs res/freedos or app bundle Resources)");
    return -1;
  }

  int rc = -1;
  char work_disk[160] = {0}, attached_disk[160] = {0}, p1[160] = {0}, mnt[PATH_MAX] = {0};
  int is_mounted = 0, used_rmount = 0, sub_allow_fixed = o->allow_fixed;

  RufuxPartOpts po = {
    .scheme = "dos", .layout = "single", .fs_main = "vfat", .dry_run = 0,
    .allow_fixed = o->allow_fixed, .allow_file = o->allow_file, .yes = 1
  };
  if (partition_and_attach_if_file(dst, &po, work_disk, sizeof work_disk,
                                   attached_disk, sizeof attached_disk,
                                   &sub_allow_fixed, err, cap) != 0)
    goto out;

  stage(prog, puser, 10);
  rufux_macos_part_slice(work_disk, 1, p1, sizeof p1);
  if (wait_node(p1, err, cap) != 0) goto out;

  (void)wipe_part_edges(p1);
  RufuxMkfsOpts mo = {
    .fs = "vfat", .label = o->label, .cluster_sectors = o->cluster_sectors,
    .dry_run = 0, .allow_fixed = sub_allow_fixed, .allow_file = 0, .yes = 1
  };
  if (rufux_format(p1, &mo, err, cap) != 0) goto out;
  stage(prog, puser, 20);

  // touch_no_index = 0 so KERNEL.SYS is strictly the very first root directory entry!
  if (mount_native_private(p1, 0, mnt, sizeof mnt, &used_rmount, err, cap) != 0) goto out;
  is_mounted = 1;

  if (rufux_dos_copy_files(fd_dir, mnt, err, cap) != 0) goto out;
  stage(prog, puser, 70);

  {
    char uerr[512] = {0};
    is_mounted = 0;
    if (unmount_native_private(p1, mnt, used_rmount, uerr, sizeof uerr) != 0) {
      snprintf(err, cap, "files copied but unmount failed: %s", uerr);
      goto out;
    }
  }

  {
    char lab[12] = {0};
    snprintf(lab, sizeof lab, "%s", o->label ? o->label : "FREEDOS");
    if (rufux_dos_pbr_fd32(p1, 0, lab, err, cap) != 0) goto out;
    if (rufux_dos_mbr(work_disk, err, cap) != 0) goto out;
  }

  stage(prog, puser, 100);
  rc = 0;

out:
  if (is_mounted) {
    char uerr[256];
    (void)unmount_native_private(p1, mnt, used_rmount, uerr, sizeof uerr);
  }
  if (attached_disk[0]) {
    char uerr[256];
    (void)rufux_macos_unmount_disk(attached_disk, uerr, sizeof uerr);
    (void)rufux_macos_detach_disk(attached_disk);
  }
  return rc;
}

static long long find_ci_size(const char *root, const char *relpath) {
  long long sz = -1;
  if (resolve_ci_path(root, relpath, NULL, 0, &sz) != 0) return -1;
  return sz;
}

static int verify_windows_tree(const char *root, int require_install,
                               RufuxCreateLog log, void *luser,
                               char *err, unsigned long cap) {
  char m[1280];
  static const char *must[] = {
    "bootmgr", "setup.exe", "sources/setup.exe", "sources/boot.wim",
    "boot/bcd", "efi/microsoft/boot/bcd", NULL
  };
  for (int i = 0; must[i]; i++) {
    if (find_ci_size(root, must[i]) <= 0) {
      snprintf(err, cap, "extraction incomplete: '%s' is missing or empty on the stick "
                         "(Windows Setup reports this as a missing media driver)", must[i]);
      return -1;
    }
  }
  static const char *loaders[] = {
    "efi/boot/bootx64.efi", "efi/boot/bootia32.efi", "efi/boot/bootaa64.efi", NULL
  };
  int have_loader = 0;
  for (int i = 0; loaders[i] && !have_loader; i++) {
    if (find_ci_size(root, loaders[i]) > 0) have_loader = 1;
  }
  if (!have_loader) {
    snprintf(err, cap, "extraction incomplete: no efi/boot/boot*.efi on the stick");
    return -1;
  }
  if (!require_install) return 0;

  static const char *inst[] = {
    "sources/install.wim", "sources/install.esd", "sources/install.swm", NULL
  };
  for (int i = 0; inst[i]; i++) {
    long long sz = find_ci_size(root, inst[i]);
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
  char m[1024];
  int gpt = strcmp(o->scheme, "dos") != 0;
  int fat = !strcmp(o->fs, "vfat") || !strcmp(o->fs, "fat32");
  const char *fsname = fat ? "vfat" : "ntfs";
  if (!fat && strcmp(o->fs, "ntfs") != 0) {
    snprintf(err, cap, "Windows media supports FAT32 (vfat) or NTFS, not '%s'", o->fs);
    return -1;
  }

  if (o->dry_run) {
    if (rufux_check_target(dst, o->allow_fixed, o->allow_file, err, cap) != 0) return -1;
    if (log) {
      snprintf(m, sizeof m,
               "steps:\n  1. partition %s %s: %s\n"
               "  2. %s\n"
               "  3. populate partition 1 from %s%s\n"
               "  4. Windows customization (%s)\n  5. verify the copied Windows tree\n"
               "  6. install legacy BIOS boot code (GRUB 2 %s, starts bootmgr)",
               dst, gpt ? "gpt" : "dos",
               fat ? (gpt ? "one FAT32 partition, 1 MiB BIOS boot last" : "one FAT32 partition")
                   : (gpt ? "NTFS data partition first, 1 MiB UEFI:NTFS, 1 MiB BIOS boot last"
                          : "NTFS data partition first, 1 MiB UEFI:NTFS last"),
               fat ? "format partition 1 as FAT32"
                   : "write UEFI:NTFS image to partition 2 (raw), format partition 1 as NTFS",
               src, fat ? " (install.wim split with wimlib if over 4 GiB)" : "",
               o->wue ? o->wue : "none",
               gpt ? "in the BIOS boot partition" : "in the MBR gap");
      log(m, luser);
    }
    return 0;
  }

  {
    RufuxWueArgs chk = {o->wue, o->locale, o->keyboard, o->timezone};
    if (rufux_windows_check(&chk, err, cap) != 0) return -1;
  }

  int rc = -1;
  char work_disk[160] = {0}, attached_disk[160] = {0}, p1[160] = {0}, p2[160] = {0};
  char mnt[PATH_MAX] = {0}, stg[PATH_MAX] = {0}, wim_stg[PATH_MAX] = {0};
  char isomnt[256] = {0}, isodev[128] = {0};
  int is_mounted = 0, used_rmount = 0, iso_mounted = 0, sub_allow_fixed = o->allow_fixed;

  RufuxPartOpts po = {
    .scheme = gpt ? "gpt" : "dos", .fs_main = fsname,
    .layout = fat ? "single" : "main+uefintfs", .bios_boot = gpt,
    .dry_run = 0, .allow_fixed = o->allow_fixed, .allow_file = o->allow_file, .yes = 1
  };
  if (partition_and_attach_if_file(dst, &po, work_disk, sizeof work_disk,
                                   attached_disk, sizeof attached_disk,
                                   &sub_allow_fixed, err, cap) != 0)
    goto out;

  stage(prog, puser, 5);
  rufux_macos_part_slice(work_disk, 1, p1, sizeof p1);
  if (wait_node(p1, err, cap) != 0) goto out;

  if (!fat) {
    rufux_macos_part_slice(work_disk, 2, p2, sizeof p2);
    if (wait_node(p2, err, cap) != 0) goto out;
    if (log) log("Writing UEFI:NTFS boot partition...", luser);
    if (rufux_write_uefi_ntfs(p2, err, cap) != 0) goto out;
  }

  if (!o->quick_format) {
    ProgMap zm = {prog, puser, 5, 3};
    if (zero_head(p1, log, luser, prog ? (RufuxCreateProgress)mapped : NULL, &zm, err, cap) != 0)
      goto out;
  }
  if (wipe_part_edges(p1) != 0) {
    snprintf(err, cap, "cannot write to '%s': %s (write-protected or failing drive?)",
             p1, strerror(errno));
    goto out;
  }

  RufuxMkfsOpts main_o = {
    .fs = fsname, .label = o->label, .cluster_sectors = o->cluster_sectors,
    .dry_run = 0, .allow_fixed = sub_allow_fixed, .allow_file = 0, .yes = 1
  };
  snprintf(m, sizeof m, "Creating file system (%s)...", fsname);
  if (log) log(m, luser);
  if (rufux_format(p1, &main_o, err, cap) != 0) goto out;
  stage(prog, puser, 10);

  if (fat) {
    if (mount_native_private(p1, 1, mnt, sizeof mnt, &used_rmount, err, cap) != 0) goto out;
    is_mounted = 1;

    ProgMap em = {prog, puser, 10, 70};
    if (rufux_extract_windows_split(src, mnt, o->split_wim_mb ? o->split_wim_mb : 3800,
                                    o->split_wim_mb != 0,
                                    prog ? (RufuxExtractProgress)mapped : NULL, &em,
                                    log, luser, err, cap) != 0)
      goto out;

    if (o->wue && strcmp(o->wue, "none") != 0) {
      RufuxWueArgs wa = {o->wue, o->locale, o->keyboard, o->timezone};
      if (rufux_windows_customize(mnt, &wa, log, luser, err, cap) != 0) goto out;
    }
    stage(prog, puser, 88);
    if (verify_windows_tree(mnt, 1, log, luser, err, cap) != 0) goto out;
    rufux_bootcheck_tree(mnt, log, luser);
    if (o->uefi_validate) uefi_validate_in_tree(mnt, 1, log, luser);
    stage(prog, puser, 90);

    char uerr[512] = {0};
    is_mounted = 0;
    if (unmount_native_private(p1, mnt, used_rmount, uerr, sizeof uerr) != 0) {
      snprintf(err, cap, "installed but unmount failed: %s", uerr);
      goto out;
    }
  } else {
    unsigned long long iso_sz = file_size(src);
    char inst_entry[256] = {0};
    unsigned long long inst_sz = 0;
    if (!o->split_wim_mb && rufux_have("7z")) {
      if (!rufux_archive_find_entry(src, "sources/install.wim",
                                    inst_entry, sizeof inst_entry, &inst_sz)) {
        (void)rufux_archive_find_entry(src, "sources/install.esd",
                                       inst_entry, sizeof inst_entry, &inst_sz);
      }
    }

    int stream_inst = (!o->split_wim_mb && inst_entry[0] && inst_sz > 0);
    unsigned long long stage_need =
        (stream_inst && iso_sz > inst_sz ? (iso_sz - inst_sz) : iso_sz) + (128ULL << 20);
    if (rufux_scratch_dir(stage_need, src, stg, sizeof stg) != 0) {
      snprintf(err, cap, "insufficient scratch space to stage Windows media (set RUFUX_TMPDIR)");
      goto out;
    }

    if (stream_inst) {
      ProgMap em = {prog, puser, 10, 22};
      if (rufux_extract_iso_excluding(src, stg, inst_entry,
                                      iso_sz > inst_sz ? iso_sz - inst_sz : 0,
                                      prog ? (RufuxExtractProgress)mapped : NULL, &em,
                                      err, cap) != 0)
        goto out;

      if (o->wue && strcmp(o->wue, "none") != 0) {
        RufuxWueArgs wa = {o->wue, o->locale, o->keyboard, o->timezone};
        if (rufux_windows_customize(stg, &wa, log, luser, err, cap) != 0) goto out;
      }
      if (verify_windows_tree(stg, 0, log, luser, err, cap) != 0) goto out;
      rufux_bootcheck_tree(stg, log, luser);
      if (o->uefi_validate) uefi_validate_in_tree(stg, 1, log, luser);

      ProgMap nm1 = {prog, puser, 35, 15};
      if (rufux_ntfs_populate_dir(p1, stg, prog ? (RufuxNtfsProgress)mapped : NULL, &nm1,
                                  err, cap) != 0)
        goto out;
      rm_rf_dir(stg);
      stg[0] = 0;

      // Stream install.wim/install.esd directly from mounted ISO if possible, else scratch copy.
      ProgMap nm2 = {prog, puser, 50, 38};
      int wrote_inst = 0;
      if (rufux_iso_mount_ro(src, isomnt, sizeof isomnt, isodev, sizeof isodev) == 0) {
        iso_mounted = 1;
        char mounted_inst[PATH_MAX];
        long long mounted_sz = -1;
        struct statfs sfs;
        int is_cd9660_over_4g = (inst_sz > 0xFFFFFFFFULL && statfs(isomnt, &sfs) == 0 &&
                                 strcmp(sfs.f_fstypename, "cd9660") == 0);
        if (!is_cd9660_over_4g &&
            resolve_ci_path(isomnt, inst_entry, mounted_inst, sizeof mounted_inst, &mounted_sz) == 0 &&
            mounted_sz > 0 && (unsigned long long)mounted_sz == inst_sz) {
          snprintf(m, sizeof m, "Streaming %s (%.1f MiB) directly into NTFS from mounted image...",
                   inst_entry, inst_sz / 1048576.0);
          if (log) log(m, luser);
          if (rufux_ntfs_write_single_file(p1, mounted_inst, inst_entry,
                                           prog ? (RufuxNtfsProgress)mapped : NULL, &nm2,
                                           err, cap) == 0) {
            wrote_inst = 1;
          }
        }
        rufux_iso_umount_ro(isomnt, isodev);
        iso_mounted = 0;
      }
      if (!wrote_inst) {
        if (rufux_scratch_dir(inst_sz + (128ULL << 20), src, wim_stg, sizeof wim_stg) != 0) {
          snprintf(err, cap, "insufficient scratch space for '%s' (set RUFUX_TMPDIR)", inst_entry);
          goto out;
        }
        ProgMap xm = {prog, puser, 50, 19};
        if (rufux_extract_single_entry(src, wim_stg, inst_entry, inst_sz,
                                       prog ? (RufuxExtractProgress)mapped : NULL, &xm,
                                       err, cap) != 0)
          goto out;
        const char *bn = strrchr(inst_entry, '/');
        bn = bn ? bn + 1 : inst_entry;
        char extracted_inst[PATH_MAX];
        long long extracted_sz = -1;
        if ((resolve_ci_path(wim_stg, bn, extracted_inst, sizeof extracted_inst, &extracted_sz) != 0 &&
             resolve_ci_path(wim_stg, inst_entry, extracted_inst, sizeof extracted_inst, &extracted_sz) != 0) ||
            extracted_sz <= 0 || (unsigned long long)extracted_sz != inst_sz) {
          snprintf(err, cap, "extracted '%s' missing or size mismatch in scratch dir", inst_entry);
          goto out;
        }
        snprintf(m, sizeof m, "Streaming %s (%.1f MiB) into NTFS from extracted scratch copy...",
                 inst_entry, inst_sz / 1048576.0);
        if (log) log(m, luser);
        ProgMap wm2 = {prog, puser, 69, 19};
        if (rufux_ntfs_write_single_file(p1, extracted_inst, inst_entry,
                                         prog ? (RufuxNtfsProgress)mapped : NULL, &wm2,
                                         err, cap) != 0)
          goto out;
        rm_rf_dir(wim_stg);
        wim_stg[0] = 0;
      }
      snprintf(m, sizeof m, "Verified %s (%.1f MiB) on the stick.",
               inst_entry, inst_sz / 1048576.0);
      if (log) log(m, luser);
      stage(prog, puser, 90);
    } else {
      ProgMap em = {prog, puser, 10, 45};
      if (o->split_wim_mb) {
        if (rufux_extract_windows_split(src, stg, o->split_wim_mb, 1,
                                        prog ? (RufuxExtractProgress)mapped : NULL, &em,
                                        log, luser, err, cap) != 0)
          goto out;
      } else {
        if (rufux_extract_iso_progress(src, stg, 0,
                                       prog ? (RufuxExtractProgress)mapped : NULL, &em,
                                       err, cap) != 0)
          goto out;
      }
      if (o->wue && strcmp(o->wue, "none") != 0) {
        RufuxWueArgs wa = {o->wue, o->locale, o->keyboard, o->timezone};
        if (rufux_windows_customize(stg, &wa, log, luser, err, cap) != 0) goto out;
      }
      if (verify_windows_tree(stg, 1, log, luser, err, cap) != 0) goto out;
      rufux_bootcheck_tree(stg, log, luser);
      if (o->uefi_validate) uefi_validate_in_tree(stg, 1, log, luser);

      ProgMap nm = {prog, puser, 58, 30};
      if (rufux_ntfs_populate_dir(p1, stg, prog ? (RufuxNtfsProgress)mapped : NULL, &nm,
                                  err, cap) != 0)
        goto out;
      rm_rf_dir(stg);
      stg[0] = 0;
      stage(prog, puser, 90);
    }
  }

  int bios_ok = 0;
  {
    char be[512] = {0};
    if (log) log("Installing legacy BIOS boot code...", luser);
    if (rufux_windows_bios_install(work_disk, NULL, log, luser, be, sizeof be) == 0) {
      bios_ok = 1;
    } else if (log) {
      snprintf(m, sizeof m,
               "WARNING: no legacy BIOS boot code: %s. The drive boots on UEFI machines only.", be);
      log(m, luser);
    }
  }

  if (!fat && rufux_have("ntfsfix")) {
    const char *nf[] = {"ntfsfix", "-d", p1, NULL};
    if (rufux_run(nf, 0) != 0 && log)
      log("WARNING: could not clear the NTFS dirty flag (ntfsfix failed).", luser);
  }

  if (log) {
    log(bios_ok ? "This drive boots on both UEFI and legacy BIOS machines."
                : "NOTE: this drive boots on UEFI machines only (no legacy BIOS boot code was installed).",
        luser);
  }
  stage(prog, puser, 100);
  rc = 0;

out:
  if (iso_mounted) rufux_iso_umount_ro(isomnt, isodev);
  if (is_mounted) {
    char uerr[256];
    (void)unmount_native_private(p1, mnt, used_rmount, uerr, sizeof uerr);
  }
  if (wim_stg[0]) rm_rf_dir(wim_stg);
  if (stg[0]) rm_rf_dir(stg);
  if (attached_disk[0]) {
    char uerr[256];
    (void)rufux_macos_unmount_disk(attached_disk, uerr, sizeof uerr);
    (void)rufux_macos_detach_disk(attached_disk);
  }
  return rc;
}

int rufux_create(const char *src, const char *dst, const RufuxCreateOpts *o,
                 RufuxCreateProgress prog, void *puser,
                 RufuxCreateLog log, void *luser,
                 char *err, unsigned long errcap) {
  if (err && errcap > 0) err[0] = 0;
  char m[1024];
  RufuxCreateOpts norm = *o;
  if (norm.scheme && !strcmp(norm.scheme, "mbr")) norm.scheme = "dos";
  o = &norm;

  snprintf(m, sizeof m, "Rufux: %s -> %s [mode=%s scheme=%s fs=%s label=%s]%s",
           src ? src : "(none)", dst, o->mode, o->scheme, o->fs,
           o->label ? o->label : "", o->dry_run ? " [dry-run]" : "");
  if (log) log(m, luser);

  if (rufux_macos_is_boot_disk(dst)) {
    snprintf(err, errcap, "refusing boot/system disk '%s'", dst);
    return -1;
  }

  if (!o->dry_run && !o->yes) {
    snprintf(err, errcap, "refusing real run without --yes (use --dry-run to plan)");
    return -1;
  }

  if (!o->dry_run && is_disk_dev(dst)) {
    if (rufux_need_root_for_block(dst, err, errcap) != 0) return -1;
    if (rufux_macos_unmount_disk(dst, err, errcap) != 0) {
      if (!err[0]) snprintf(err, errcap, "cannot unmount %s (close files and retry)", dst);
      return -1;
    }
  }

  if (is_disk_dev(dst)) {
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

  if (o->badblock_passes > 0 && !o->dry_run) {
    unsigned span = !strcmp(o->mode, "dd") ? 10 : 8;
    if (run_badblocks(dst, o->badblock_passes, o->allow_file, 0, span,
                      prog, puser, log, luser, err, errcap) != 0)
      return -1;
  } else if (o->badblock_passes > 0 && log) {
    snprintf(m, sizeof m, "+ bad-blocks check %d pass(es) on %s [dry-run]",
             o->badblock_passes, dst);
    log(m, luser);
  }

  if (!o->dry_run && !o->quick_format && is_disk_dev(dst) && !strcmp(o->mode, "dd")) {
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
    if (!src) {
      snprintf(err, errcap, "windows mode needs an image");
      return -1;
    }
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

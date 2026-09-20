#define _GNU_SOURCE
#include "partition.h"
#include "device.h"
#include "exec.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <linux/fs.h>

// Size of a block device or image file in bytes (0 on failure).
static unsigned long long target_bytes(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return 0;
  if (S_ISREG(st.st_mode)) return (unsigned long long)st.st_size;
  unsigned long long b = 0;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    if (ioctl(fd, BLKGETSIZE64, &b) != 0) b = 0;
    close(fd);
  }
  if (b == 0) {
    // sysfs keeps a sector count even where the ioctl is unavailable.
    const char *base = strrchr(path, '/');
    char sp[256], buf[64] = {0};
    snprintf(sp, sizeof sp, "/sys/class/block/%s/size", base ? base + 1 : path);
    FILE *f = fopen(sp, "r");
    if (f) {
      if (fgets(buf, sizeof buf, f)) b = strtoull(buf, NULL, 10) * 512ULL;
      fclose(f);
    }
  }
  return b;
}

void rufux_partition_plan(const char *dst, const RufuxPartOpts *o,
                          char *out, unsigned long cap) {
  snprintf(out, cap, "partition %s as %s/%s (sfdisk)%s", dst, o->scheme,
           o->layout, o->dry_run ? " [dry-run]" : "");
}

// sfdisk only ever writes the partition table and the 55AA signature; the
// 440-byte bootstrap-code region ahead of it (offset 0) is left all zero.
// A dos/MBR disk with zeroed boot code is a structurally valid MBR, but it
// is not what any real MBR-writing tool produces: Rufus always embeds a
// real x86 bootstrap there (mbr_win7.h/mbr_rufus.h - actual compiled
// machine code) for exactly this layout, and Ventoy's own MBR carries its
// grub-based bootstrap the same way. UEFI firmware never executes this
// region, so it cannot be why the initial boot fails, but it is a real,
// verified gap between what Rufux writes and what every other tool in this
// space writes, and it costs nothing to close: dd the standard, GPL,
// freely redistributable syslinux mbr.bin over just those 440 bytes,
// leaving the disk signature, reserved word and partition table (which
// sfdisk just wrote at 440-509) and the boot signature at 510-511 alone.
static void write_mbr_bootcode(const char *dst) {
  static const char *candidates[] = {
    "/usr/lib/syslinux/mbr/mbr.bin",       // Debian/Ubuntu
    "/usr/lib/syslinux/bios/mbr.bin",      // Arch
    "/usr/share/syslinux/mbr.bin",         // Fedora/openSUSE
    "/usr/lib/SYSLINUX/mbr.bin",
    NULL
  };
  unsigned char code[440] = {0};
  int have = 0;
  for (int i = 0; candidates[i] && !have; i++) {
    FILE *f = fopen(candidates[i], "rb");
    if (!f) continue;
    size_t n = fread(code, 1, sizeof code, f);
    fclose(f);
    if (n > 0) have = 1;
  }
  if (!have) return;  // no syslinux on this host: leave the region zeroed, as before
  int fd = open(dst, O_WRONLY | O_CLOEXEC);
  if (fd < 0) return;
  ssize_t w = pwrite(fd, code, sizeof code, 0);
  (void)w;  // best effort: a disk still boots (via UEFI) without this
  fsync(fd);
  close(fd);
}

int rufux_partition(const char *dst, const RufuxPartOpts *o,
                    char *err, unsigned long cap) {
  if (strcmp(o->scheme, "gpt") && strcmp(o->scheme, "dos") && strcmp(o->scheme, "mbr")) {
    snprintf(err, cap, "scheme must be gpt|dos (mbr alias dos)");
    return -1;
  }
  const char *scheme = !strcmp(o->scheme, "mbr") ? "dos" : o->scheme;
  if (strcmp(o->layout, "single") && strcmp(o->layout, "esp+main") &&
      strcmp(o->layout, "main+uefintfs")) {
    snprintf(err, cap, "layout must be single|esp+main|main+uefintfs");
    return -1;
  }
  if (!o->dry_run && !o->yes) {
    snprintf(err, cap, "refusing real partition without --yes");
    return -1;
  }
  if (rufux_check_target(dst, o->allow_fixed, o->allow_file, err, cap) != 0)
    return -1;
  if (!rufux_have("sfdisk")) {
    snprintf(err, cap, "sfdisk not found");
    return -1;
  }
  // Explicit field syntax (works across sfdisk generations):
  // single: one data partition (basic data type, Linux type for ext*); esp+main: 512MiB ESP + rest.
  // ESP type GUID C12A7328-F81F-11D2-BA4B-00A0C93EC93B (was a placeholder).
  // MBR type follows the filesystem: BIOS boots FAT/NTFS partitions,
  // never type 83 (Linux). gpt layouts need no boot flag (UEFI ignores
  // it); every dos/MBR data partition gets flagged bootable.
  const char *mbr_type = "0c";
  if (o->fs_main) {
    if (!strcmp(o->fs_main, "ntfs") || !strcmp(o->fs_main, "exfat") ||
        !strcmp(o->fs_main, "udf"))
      mbr_type = "07";
    else if (!strcmp(o->fs_main, "ext4") || !strcmp(o->fs_main, "ext2") ||
             !strcmp(o->fs_main, "ext3"))
      mbr_type = "83";
  }
  char script[1024];
  if (!strcmp(o->layout, "main+uefintfs")) {
    // Rufus layout for NTFS/exFAT install media that must also boot UEFI:
    // the data partition FIRST, a 1 MiB UEFI:NTFS partition LAST.
    //  - Windows Setup looks for sources\install.wim on the volumes it can
    //    mount; the data volume has to be the first partition, which is the
    //    only one older WinPE builds show on a removable drive.
    //  - The UEFI:NTFS partition must NOT carry the EFI System type:
    //    Setup refuses to copy files when the disk has two ESPs (Rufus
    //    drive.c). It is typed basic data and flagged no-drive-letter
    //    (GPT attribute bit 63) so Windows never assigns it a letter.
    // Sizes are in whole MiB so the script is identical on 512e and 4Kn
    // media. One MiB of slack at the end covers the backup GPT.
    unsigned long long bytes = target_bytes(dst);
    unsigned long long mib = bytes >> 20;
    if (mib < 64) {
      snprintf(err, cap, "target '%s' reports %llu MiB; the Windows layout needs at least 64 MiB "
                         "(an empty card-reader slot reports 0)", dst, mib);
      return -1;
    }
    unsigned long long main_mib = mib - 3; // 1 leading + 1 UEFI:NTFS + 1 slack
    if (!strcmp(scheme, "gpt"))
      snprintf(script, sizeof script,
               "label: gpt\n"
               "start=1MiB, size=%lluMiB, type=EBD0A0A2-B938-11D2-B3FA-00A0C93EC93B, name=\"Windows\"\n"
               "size=1MiB, type=EBD0A0A2-B938-11D2-B3FA-00A0C93EC93B, name=\"UEFI:NTFS\", attrs=\"GUID:63\"\n",
               main_mib);
    else
      snprintf(script, sizeof script,
               "label: dos\nstart=1MiB, size=%lluMiB, type=%s, bootable\nsize=1MiB, type=ef\n",
               main_mib, mbr_type);
  } else if (!strcmp(o->layout, "single")) {
    if (!strcmp(scheme, "gpt")) {
      // Windows only mounts Microsoft basic data partitions; the Linux
      // filesystem type is right for ext* alone.
      const char *gtype = !strcmp(mbr_type, "83") ? "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
                                                   : "EBD0A0A2-B938-11D2-B3FA-00A0C93EC93B";
      snprintf(script, sizeof script, "label: gpt\nstart=1MiB, type=%s\n", gtype);
    }
    else
      snprintf(script, sizeof script, "label: dos\nstart=1MiB, type=%s, bootable\n",
               mbr_type);
  } else if (!strcmp(scheme, "gpt")) {
    // ESP + Windows/Linux data partition: the data volume carries a
    // Windows-readable filesystem (NTFS/exFAT), so like Rufus it gets
    // the Microsoft basic data type, not the Linux filesystem type.
    snprintf(script, sizeof script,
             "label: gpt\nsize=512MiB, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B\n"
             "type=EBD0A0A2-B938-11D2-B3FA-00A0C93EC93B\n");
  } else {
    snprintf(script, sizeof script,
             "label: dos\nsize=512MiB, type=ef\ntype=%s, bootable\n", mbr_type);
  }

  if (o->dry_run) {
    fprintf(stderr, "+ sfdisk --wipe always --wipe-partitions always %s <<'%s'\n", dst, script);
    return 0;
  }
  // write script to temp and run sfdisk < script (no shell)
  char tmpl[] = "/tmp/rufux-sfdisk-XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) { snprintf(err, cap, "mkstemp failed"); return -1; }
  size_t L = strlen(script);
  if (write(fd, script, L) != (ssize_t)L) { close(fd); unlink(tmpl); snprintf(err, cap, "tmp write failed"); return -1; }
  close(fd);
  // --wipe always clears signatures found on the *disk*; --wipe-partitions
  // always clears the ones sitting inside each newly created partition.
  // Without the second flag sfdisk only warns ("Partition #1 contains a vfat
  // signature") and leaves the old FAT/NTFS superblock and its backup copies
  // in place, so probers (and Windows) can still latch onto the stale volume.
  const char *argv[] = {"sfdisk", "--wipe", "always",
                        "--wipe-partitions", "always", dst, NULL};
  // redirect stdin from tmpl
  int rc = 0;
  pid_t p = fork();
  if (p < 0) rc = -1;
  else if (p == 0) {
    FILE *f = freopen(tmpl, "r", stdin);
    (void)f;
    execvp(argv[0], (char *const *)argv);
    _exit(127);
  } else {
    int st = 0;
    while (waitpid(p, &st, 0) < 0) {}
    rc = (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
  }
  unlink(tmpl);
  // Tell the kernel to rescan (best effort; failures are non-fatal).
  // NOTE: dry_run here is 0 = really run. A previous revision passed 1
  // (log-only), which left standalone `partition` with a stale table.
  {
    const char *pa[] = {"partprobe", dst, NULL};
    if (rufux_have("partprobe")) rufux_run(pa, 0);
    const char *us[] = {"udevadm", "settle", NULL};
    if (rufux_have("udevadm")) rufux_run(us, 0);
  }
  if (rc != 0) snprintf(err, cap, "sfdisk failed on '%s'", dst);
  if (rc == 0 && !strcmp(scheme, "dos")) write_mbr_bootcode(dst);
  return rc;
}

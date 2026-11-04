#include "platform_macos.h"
#include "exec.h"
#include "mount.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOMedia.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

static int is_dir(const char *p) {
  struct stat st;
  return p && p[0] && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_file(const char *p) {
  struct stat st;
  return p && p[0] && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

const char *rufux_macos_exe_dir(void) {
  static char dir[PATH_MAX] = {0};
  static int done = 0;
  if (!done) {
    done = 1;
    char raw[PATH_MAX * 2];
    uint32_t sz = (uint32_t)sizeof raw;
    char *buf = raw;
    char *heap = NULL;
    if (_NSGetExecutablePath(buf, &sz) != 0) {
      heap = malloc(sz);
      if (heap && _NSGetExecutablePath(heap, &sz) == 0) buf = heap;
      else buf = NULL;
    }
    if (buf) {
      char resolved[PATH_MAX];
      const char *use = realpath(buf, resolved) ? resolved : buf;
      snprintf(dir, sizeof dir, "%s", use);
      char *slash = strrchr(dir, '/');
      if (slash) {
        if (slash == dir) dir[1] = 0;
        else *slash = 0;
      } else {
        dir[0] = 0;
      }
    }
    free(heap);
  }
  return dir;
}

static int path_contains_entry(const char *path, const char *entry) {
  if (!path || !entry || !entry[0]) return 0;
  size_t elen = strlen(entry);
  const char *p = path;
  while (*p) {
    const char *colon = strchr(p, ':');
    size_t len = colon ? (size_t)(colon - p) : strlen(p);
    if (len == elen && strncmp(p, entry, len) == 0) return 1;
    if (!colon) break;
    p = colon + 1;
  }
  return 0;
}

static void path_prepend(char *buf, size_t cap, const char *dir) {
  if (!is_dir(dir) || path_contains_entry(buf, dir)) return;
  char tmp[8192];
  if (buf[0]) snprintf(tmp, sizeof tmp, "%s:%s", dir, buf);
  else snprintf(tmp, sizeof tmp, "%s", dir);
  snprintf(buf, cap, "%s", tmp);
}

static void path_append(char *buf, size_t cap, const char *dir) {
  if (!is_dir(dir) || path_contains_entry(buf, dir)) return;
  size_t len = strlen(buf);
  if (len == 0) snprintf(buf, cap, "%s", dir);
  else if (len + 1 + strlen(dir) < cap) snprintf(buf + len, cap - len, ":%s", dir);
}

void rufux_macos_init_bundle_env(void) {
  const char *ed = rufux_macos_exe_dir();
  const char *cur = getenv("PATH");
  char new_path[8192];
  snprintf(new_path, sizeof new_path, "%s",
           (cur && cur[0]) ? cur : "/usr/bin:/bin:/usr/sbin:/sbin");

  int in_app_bundle = 0;
  char contents_dir[PATH_MAX] = {0};

  if (ed[0]) {
    // Case 1: <Bundle>.app/Contents/MacOS/rufux -> Contents is <ed>/..
    // Case 2: <Bundle>.app/Contents/Resources/bin/rufux -> Contents is <ed>/../..
    const char *rel_candidates[] = {"..", "../..", NULL};
    for (int i = 0; rel_candidates[i]; i++) {
      char cand[PATH_MAX * 2];
      char resolved[PATH_MAX];
      snprintf(cand, sizeof cand, "%s/%s", ed, rel_candidates[i]);
      if (!realpath(cand, resolved)) continue;
      char plist[PATH_MAX * 2];
      char res_bin[PATH_MAX * 2];
      snprintf(plist, sizeof plist, "%s/Info.plist", resolved);
      snprintf(res_bin, sizeof res_bin, "%s/Resources/bin", resolved);
      if (is_file(plist) && is_dir(res_bin)) {
        snprintf(contents_dir, sizeof contents_dir, "%s", resolved);
        in_app_bundle = 1;
        break;
      }
    }

    // Always honour <ed> and an adjacent ../Resources/bin when present.
    path_prepend(new_path, sizeof new_path, ed);
    char adj_res_bin[PATH_MAX * 2], adj_resolved[PATH_MAX];
    snprintf(adj_res_bin, sizeof adj_res_bin, "%s/../Resources/bin", ed);
    if (realpath(adj_res_bin, adj_resolved))
      path_prepend(new_path, sizeof new_path, adj_resolved);
  }

  if (in_app_bundle) {
    char res_bin[PATH_MAX * 2];
    char res_share[PATH_MAX * 2];
    char res_grub[PATH_MAX * 2];
    snprintf(res_bin, sizeof res_bin, "%s/Resources/bin", contents_dir);
    snprintf(res_share, sizeof res_share, "%s/Resources/share/rufux", contents_dir);
    snprintf(res_grub, sizeof res_grub, "%s/Resources/lib/grub/i386-pc", contents_dir);

    path_prepend(new_path, sizeof new_path, res_bin);

    const char *cur_res = getenv("RUFUX_RES");
    if ((!cur_res || !cur_res[0]) && is_dir(res_share))
      setenv("RUFUX_RES", res_share, 1);

    const char *cur_grub = getenv("RUFUX_GRUB_DIR");
    if ((!cur_grub || !cur_grub[0]) && is_dir(res_grub))
      setenv("RUFUX_GRUB_DIR", res_grub, 1);
  } else {
    // Development / CTest environment: locate macos/vendor/ntfs-3g/{bin,sbin}
    // relative to the executable, plus Homebrew keg-only e2fsprogs and bin.
    if (ed[0]) {
      const char *levels[] = {"..", "../..", "../../..", NULL};
      for (int i = 0; levels[i]; i++) {
        char vbase[PATH_MAX * 2], vresolved[PATH_MAX];
        snprintf(vbase, sizeof vbase, "%s/%s/vendor/ntfs-3g", ed, levels[i]);
        if (realpath(vbase, vresolved)) {
          char vbin[PATH_MAX * 2], vsbin[PATH_MAX * 2];
          snprintf(vbin, sizeof vbin, "%s/bin", vresolved);
          snprintf(vsbin, sizeof vsbin, "%s/sbin", vresolved);
          path_append(new_path, sizeof new_path, vbin);
          path_append(new_path, sizeof new_path, vsbin);
          break;
        }
      }
    }
    path_append(new_path, sizeof new_path, "/opt/homebrew/opt/e2fsprogs/sbin");
    path_append(new_path, sizeof new_path, "/opt/homebrew/opt/e2fsprogs/bin");
    path_append(new_path, sizeof new_path, "/opt/homebrew/bin");
    path_append(new_path, sizeof new_path, "/opt/homebrew/sbin");
  }

  setenv("PATH", new_path, 1);
}

// Returns 1 iff s matches disk<digits>(s<digits>)*.
static int is_valid_bsd_disk_ident(const char *s) {
  if (!s || strncmp(s, "disk", 4) != 0 || !isdigit((unsigned char)s[4])) return 0;
  size_t i = 4;
  for (;;) {
    if (!isdigit((unsigned char)s[i])) return 0;
    while (isdigit((unsigned char)s[i])) i++;
    if (s[i] == '\0') return 1;
    if (s[i] == 's') { i++; continue; }
    return 0;
  }
}

static int extract_bsd_from_str(const char *p, char *bsd_out, size_t cap) {
  if (!p || !p[0] || !bsd_out || cap == 0) return 0;
  bsd_out[0] = 0;
  if (strncmp(p, "/private/dev/", 13) == 0) p += 8;
  const char *cand = NULL;
  if (strncmp(p, "/dev/rdisk", 10) == 0) cand = p + 6;
  else if (strncmp(p, "/dev/disk", 9) == 0) cand = p + 5;
  else if (strchr(p, '/') == NULL) {
    if (strncmp(p, "rdisk", 5) == 0) cand = p + 1;
    else if (strncmp(p, "disk", 4) == 0) cand = p;
  }
  if (!cand || !is_valid_bsd_disk_ident(cand)) return 0;
  snprintf(bsd_out, cap, "%s", cand);
  return 1;
}

static int resolve_to_bsd_name(const char *path, char *bsd_out, size_t cap) {
  if (!path || !path[0] || !bsd_out || cap == 0) return 0;
  bsd_out[0] = 0;

  char resolved[PATH_MAX];
  if (realpath(path, resolved)) {
    struct stat st;
    if (stat(resolved, &st) == 0) {
      if (S_ISREG(st.st_mode)) return 0;
      if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) {
        if (extract_bsd_from_str(resolved, bsd_out, cap)) return 1;
        char dname[64] = {0};
        if (devname_r(st.st_rdev, S_ISCHR(st.st_mode) ? S_IFCHR : S_IFBLK,
                      dname, (int)sizeof dname) &&
            extract_bsd_from_str(dname, bsd_out, cap)) {
          return 1;
        }
      }
    }
    if (extract_bsd_from_str(resolved, bsd_out, cap)) return 1;
  }
  return extract_bsd_from_str(path, bsd_out, cap);
}

static void whole_bsd_prefix(const char *bsd, char *whole_out, size_t cap) {
  if (!whole_out || cap == 0) return;
  whole_out[0] = 0;
  if (!bsd || strncmp(bsd, "disk", 4) != 0 || !isdigit((unsigned char)bsd[4])) return;
  size_t i = 0;
  while (bsd[i] && (i < 4 || isdigit((unsigned char)bsd[i])) && i + 1 < cap) {
    whole_out[i] = bsd[i];
    i++;
  }
  whole_out[i] = 0;
}

static void bsd_set_add_one(char set[][64], int *count, int max_set, const char *name) {
  if (!name || !name[0] || *count >= max_set) return;
  for (int i = 0; i < *count; i++) {
    if (strcmp(set[i], name) == 0) return;
  }
  snprintf(set[*count], 64, "%s", name);
  (*count)++;
}

static void bsd_set_add(char set[][64], int *count, int max_set, const char *name) {
  if (!name || !name[0]) return;
  bsd_set_add_one(set, count, max_set, name);
  char whole[64] = {0};
  whole_bsd_prefix(name, whole, sizeof whole);
  if (whole[0]) bsd_set_add_one(set, count, max_set, whole);
}

// Walks IOKit kIOServicePlane parents upward from `bsd` (handling APFS snapshots,
// APFS volumes, AppleAPFSContainer, AppleAPFSContainerScheme with 1+ physical stores,
// partition slices, and physical whole-disk IOMedia) collecting all ancestor BSD names.
static int collect_iokit_lineage_bsds(const char *bsd, char set[][64], int max_set) {
  int count = 0;
  bsd_set_add(set, &count, max_set, bsd);

  io_service_t start = IOServiceGetMatchingService(
      kIOMainPortDefault, IOBSDNameMatching(kIOMainPortDefault, 0, bsd));
  if (start == MACH_PORT_NULL) {
    char whole[64] = {0};
    whole_bsd_prefix(bsd, whole, sizeof whole);
    if (whole[0] && strcmp(whole, bsd) != 0) {
      start = IOServiceGetMatchingService(
          kIOMainPortDefault, IOBSDNameMatching(kIOMainPortDefault, 0, whole));
    }
  }
  if (start == MACH_PORT_NULL) return count;

  io_registry_entry_t queue[64];
  uint64_t seen_ids[64];
  int qhead = 0, qtail = 0, nseen = 0;

  uint64_t sid = 0;
  IORegistryEntryGetRegistryEntryID(start, &sid);
  if (sid != 0) seen_ids[nseen++] = sid;
  queue[qtail++] = start;

  while (qhead < qtail) {
    io_registry_entry_t cur = queue[qhead++];
    CFTypeRef bsd_cf = IORegistryEntryCreateCFProperty(
        cur, CFSTR(kIOBSDNameKey), kCFAllocatorDefault, 0);
    if (bsd_cf) {
      if (CFGetTypeID(bsd_cf) == CFStringGetTypeID()) {
        char cstr[64] = {0};
        if (CFStringGetCString((CFStringRef)bsd_cf, cstr, sizeof cstr,
                               kCFStringEncodingUTF8) &&
            is_valid_bsd_disk_ident(cstr)) {
          bsd_set_add(set, &count, max_set, cstr);
        }
      }
      CFRelease(bsd_cf);
    }

    io_iterator_t piter = MACH_PORT_NULL;
    if (IORegistryEntryGetParentIterator(cur, kIOServicePlane, &piter) == KERN_SUCCESS) {
      io_registry_entry_t parent;
      while ((parent = IOIteratorNext(piter)) != MACH_PORT_NULL) {
        uint64_t pid = 0;
        IORegistryEntryGetRegistryEntryID(parent, &pid);
        int dup = 0;
        if (pid != 0) {
          for (int k = 0; k < nseen; k++) {
            if (seen_ids[k] == pid) { dup = 1; break; }
          }
        }
        if (!dup && qtail < 64) {
          if (pid != 0 && nseen < 64) seen_ids[nseen++] = pid;
          queue[qtail++] = parent;
        } else {
          IOObjectRelease(parent);
        }
      }
      IOObjectRelease(piter);
    }
  }

  for (int k = 0; k < qtail; k++) IOObjectRelease(queue[k]);
  return count;
}

void rufux_macos_raw_node(const char *dev, char *out, size_t cap) {
  if (!out || cap == 0) return;
  if (!dev) { out[0] = 0; return; }
  char tmp[PATH_MAX];
  if (strncmp(dev, "/dev/disk", 9) == 0 && isdigit((unsigned char)dev[9]))
    snprintf(tmp, sizeof tmp, "/dev/rdisk%s", dev + 9);
  else
    snprintf(tmp, sizeof tmp, "%s", dev);
  snprintf(out, cap, "%s", tmp);
}

void rufux_macos_block_node(const char *dev, char *out, size_t cap) {
  if (!out || cap == 0) return;
  if (!dev) { out[0] = 0; return; }
  char tmp[PATH_MAX];
  if (strncmp(dev, "/dev/rdisk", 10) == 0 && isdigit((unsigned char)dev[10]))
    snprintf(tmp, sizeof tmp, "/dev/disk%s", dev + 10);
  else
    snprintf(tmp, sizeof tmp, "%s", dev);
  snprintf(out, cap, "%s", tmp);
}

void rufux_macos_part_slice(const char *disk, int slice, char *out, size_t cap) {
  if (!out || cap == 0) return;
  if (!disk) { out[0] = 0; return; }
  char tmp[PATH_MAX];
  if (slice > 0) snprintf(tmp, sizeof tmp, "%ss%d", disk, slice);
  else snprintf(tmp, sizeof tmp, "%s", disk);
  snprintf(out, cap, "%s", tmp);
}

unsigned long long rufux_macos_dev_bytes(const char *path) {
  if (!path || !path[0]) return 0;
  struct stat st;
  if (stat(path, &st) != 0) return 0;
  if (S_ISREG(st.st_mode)) return (unsigned long long)st.st_size;
  if (!S_ISBLK(st.st_mode) && !S_ISCHR(st.st_mode)) return 0;

  int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    char bnode[PATH_MAX];
    rufux_macos_block_node(path, bnode, sizeof bnode);
    if (strcmp(bnode, path) != 0)
      fd = open(bnode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  }
  if (fd >= 0) {
    uint64_t bc = 0;
    uint32_t bs = 0;
    int ok = (ioctl(fd, DKIOCGETBLOCKCOUNT, &bc) == 0 &&
              ioctl(fd, DKIOCGETBLOCKSIZE, &bs) == 0 &&
              bc > 0 && bs > 0);
    close(fd);
    if (ok) return (unsigned long long)bc * (unsigned long long)bs;
  }

  // Fallback to IOKit IOMedia Size (world-readable even when /dev/disk* is root:operator).
  char bsd[64] = {0};
  if (resolve_to_bsd_name(path, bsd, sizeof bsd)) {
    io_service_t svc = IOServiceGetMatchingService(
        kIOMainPortDefault, IOBSDNameMatching(kIOMainPortDefault, 0, bsd));
    if (svc != MACH_PORT_NULL) {
      unsigned long long bytes = 0;
      CFTypeRef sz_cf = IORegistryEntryCreateCFProperty(
          svc, CFSTR(kIOMediaSizeKey), kCFAllocatorDefault, 0);
      if (sz_cf) {
        if (CFGetTypeID(sz_cf) == CFNumberGetTypeID()) {
          int64_t v = 0;
          if (CFNumberGetValue((CFNumberRef)sz_cf, kCFNumberSInt64Type, &v) && v > 0)
            bytes = (unsigned long long)v;
        }
        CFRelease(sz_cf);
      }
      IOObjectRelease(svc);
      if (bytes > 0) return bytes;
    }
  }
  return 0;
}

unsigned rufux_macos_sector_size(const char *path) {
  if (!path || !path[0]) return 512u;
  struct stat st;
  if (stat(path, &st) != 0 || S_ISREG(st.st_mode)) return 512u;
  if (!S_ISBLK(st.st_mode) && !S_ISCHR(st.st_mode)) return 512u;

  int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    char bnode[PATH_MAX];
    rufux_macos_block_node(path, bnode, sizeof bnode);
    if (strcmp(bnode, path) != 0)
      fd = open(bnode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  }
  if (fd >= 0) {
    uint32_t bs = 0;
    int ok = (ioctl(fd, DKIOCGETBLOCKSIZE, &bs) == 0 && bs >= 512u);
    close(fd);
    if (ok) return (unsigned)bs;
  }

  char bsd[64] = {0};
  if (resolve_to_bsd_name(path, bsd, sizeof bsd)) {
    io_service_t svc = IOServiceGetMatchingService(
        kIOMainPortDefault, IOBSDNameMatching(kIOMainPortDefault, 0, bsd));
    if (svc != MACH_PORT_NULL) {
      unsigned sector = 0;
      CFTypeRef bs_cf = IORegistryEntryCreateCFProperty(
          svc, CFSTR(kIOMediaPreferredBlockSizeKey), kCFAllocatorDefault, 0);
      if (bs_cf) {
        if (CFGetTypeID(bs_cf) == CFNumberGetTypeID()) {
          int64_t v = 0;
          if (CFNumberGetValue((CFNumberRef)bs_cf, kCFNumberSInt64Type, &v) && v >= 512)
            sector = (unsigned)v;
        }
        CFRelease(bs_cf);
      }
      IOObjectRelease(svc);
      if (sector >= 512u) return sector;
    }
  }
  return 512u;
}

int rufux_macos_is_boot_disk(const char *path) {
  char cand_bsd[64] = {0};
  if (!resolve_to_bsd_name(path, cand_bsd, sizeof cand_bsd)) return 0;

  // Defense-in-depth floor: disk0 and disk0s* are always treated as boot disk.
  if (strncmp(cand_bsd, "disk0", 5) == 0 &&
      (cand_bsd[5] == '\0' || cand_bsd[5] == 's')) {
    return 1;
  }

  struct statfs sfs;
  if (statfs("/", &sfs) != 0) return 1; // fail-closed

  char root_bsd[64] = {0};
  if (!resolve_to_bsd_name(sfs.f_mntfromname, root_bsd, sizeof root_bsd))
    return 1; // fail-closed

  char root_set[32][64];
  int nroot = collect_iokit_lineage_bsds(root_bsd, root_set, 32);
  if (nroot <= 0) return 1; // fail-closed

  char cand_set[32][64];
  int ncand = collect_iokit_lineage_bsds(cand_bsd, cand_set, 32);
  for (int i = 0; i < ncand; i++) {
    for (int j = 0; j < nroot; j++) {
      if (strcmp(cand_set[i], root_set[j]) == 0) return 1;
    }
  }
  return 0;
}

int rufux_macos_unmount_disk(const char *disk, char *err, size_t cap) {
  if (err && cap > 0) err[0] = 0;
  if (!disk || !disk[0]) {
    if (err && cap > 0) snprintf(err, cap, "empty disk path");
    return -1;
  }
  if (rufux_macos_is_boot_disk(disk)) {
    if (err && cap > 0) snprintf(err, cap, "refusing to unmount boot/system disk '%s'", disk);
    return -1;
  }
  struct stat st;
  if (stat(disk, &st) != 0) {
    if (err && cap > 0) snprintf(err, cap, "target '%s' does not exist", disk);
    return -1;
  }
  if (!S_ISBLK(st.st_mode) && !S_ISCHR(st.st_mode)) {
    if (err && cap > 0) snprintf(err, cap, "refusing to unmount non-device '%s'", disk);
    return -1;
  }

  char bnode[PATH_MAX];
  rufux_macos_block_node(disk, bnode, sizeof bnode);
  char uerr[512] = {0};
  if (rufux_unmount_all_for_disk(bnode, uerr, sizeof uerr) != 0) {
    if (err && cap > 0)
      snprintf(err, cap, "%s", uerr[0] ? uerr : "failed to unmount active rufux mount");
    return -1;
  }
  const char *av[] = {"/usr/sbin/diskutil", "unmountDisk", "force", bnode, NULL};
  char out[2048] = {0};
  if (rufux_capture(av, out, sizeof out) != 0) {
    out[strcspn(out, "\r\n")] = 0;
    if (err && cap > 0)
      snprintf(err, cap, "diskutil unmountDisk failed on '%s': %s",
               bnode, out[0] ? out : "unknown error");
    return -1;
  }
  return 0;
}

int rufux_macos_attach_raw_image(const char *img_path, int readonly, char *disk_out, size_t cap) {
  if (!disk_out || cap == 0) return -1;
  disk_out[0] = 0;
  if (!img_path || !img_path[0]) return -1;

  struct stat st;
  if (stat(img_path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;

  const char *av[] = {
    "/usr/bin/hdiutil", "attach",
    "-imagekey", "diskimage-class=CRawDiskImage",
    "-nomount",
    readonly ? "-readonly" : "-readwrite",
    img_path,
    NULL
  };
  char out[4096] = {0};
  if (rufux_capture(av, out, sizeof out) != 0) return -1;

  for (const char *p = out; (p = strstr(p, "/dev/disk")) != NULL; p += 9) {
    if (p > out && !isspace((unsigned char)p[-1])) continue;
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

int rufux_macos_detach_disk(const char *disk_node) {
  if (!disk_node || !disk_node[0]) return -1;
  if (rufux_macos_is_boot_disk(disk_node)) return -1;

  char bnode[PATH_MAX];
  rufux_macos_block_node(disk_node, bnode, sizeof bnode);
  const char *av[] = {"/usr/bin/hdiutil", "detach", "-force", bnode, NULL};
  char out[2048] = {0};
  return rufux_capture(av, out, sizeof out) == 0 ? 0 : -1;
}

#include "ntfs_populate.h"
#include "exec.h"
#include "platform_macos.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ntfs-3g/attrib.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/ntfstime.h>
#include <ntfs-3g/types.h>
#include <ntfs-3g/unistr.h>
#include <ntfs-3g/volume.h>

#define NTFS_STREAM_CHUNK_BYTES (1024u * 1024u)
#define NTFS_MAX_PATH_DEPTH 64

typedef struct {
  unsigned char *buf;
  size_t buf_cap;
  unsigned long long done_bytes;
  unsigned long long total_bytes;
  RufuxNtfsProgress prog;
  void *user;
} NtfsCopyCtx;

__attribute__((format(printf, 3, 4)))
static void set_err(char *err, unsigned long cap, const char *fmt, ...) {
  if (!err || cap == 0) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, cap, fmt, ap);
  va_end(ap);
}

static int parse_bsd_ident(const char *path, char *bsd_out, size_t bcap, int *is_slice_out) {
  if (bsd_out && bcap > 0) bsd_out[0] = 0;
  if (is_slice_out) *is_slice_out = 0;
  if (!path || !path[0]) return 0;

  char resolved[PATH_MAX];
  const char *p = realpath(path, resolved) ? resolved : path;
  if (strncmp(p, "/private/dev/", 13) == 0) p += 8;

  const char *cand = NULL;
  if (strncmp(p, "/dev/rdisk", 10) == 0) cand = p + 6;
  else if (strncmp(p, "/dev/disk", 9) == 0) cand = p + 5;
  else return 0;

  if (strncmp(cand, "disk", 4) != 0 || !isdigit((unsigned char)cand[4])) return 0;
  size_t i = 4;
  while (isdigit((unsigned char)cand[i])) i++;
  if (cand[i] == '\0') {
    if (bsd_out && bcap > 0) snprintf(bsd_out, bcap, "%s", cand);
    if (is_slice_out) *is_slice_out = 0;
    return 1;
  }
  if (cand[i] != 's' || !isdigit((unsigned char)cand[i + 1])) return 0;
  size_t j = i + 1;
  while (isdigit((unsigned char)cand[j])) j++;
  if (cand[j] != '\0') return 0;

  if (bsd_out && bcap > 0) snprintf(bsd_out, bcap, "%s", cand);
  if (is_slice_out) *is_slice_out = 1;
  return 1;
}

static void unmount_if_mounted_ntfs_target(const char *dev_path) {
  char target_bsd[64];
  int is_slice = 0;
  if (!parse_bsd_ident(dev_path, target_bsd, sizeof target_bsd, &is_slice)) return;

  struct statfs *mnts = NULL;
  int n = getmntinfo(&mnts, MNT_NOWAIT);
  if (n <= 0 || !mnts) return;

  int hit = 0;
  size_t tlen = strlen(target_bsd);
  for (int i = 0; i < n; i++) {
    char mbsd[64];
    int ms = 0;
    if (!parse_bsd_ident(mnts[i].f_mntfromname, mbsd, sizeof mbsd, &ms)) continue;
    if (is_slice) {
      if (strcmp(mbsd, target_bsd) == 0) { hit = 1; break; }
    } else {
      if (strncmp(mbsd, target_bsd, tlen) == 0 &&
          (mbsd[tlen] == '\0' || mbsd[tlen] == 's')) {
        hit = 1;
        break;
      }
    }
  }
  if (!hit) return;

  char bnode[160], out[512];
  rufux_macos_block_node(dev_path, bnode, sizeof bnode);
  if (is_slice) {
    const char *av[] = {"/usr/sbin/diskutil", "unmount", "force", bnode, NULL};
    (void)rufux_capture(av, out, sizeof out);
  } else {
    (void)rufux_macos_unmount_disk(bnode, out, sizeof out);
  }
}

static ntfs_volume *open_ntfs_rw(const char *ntfs_target, char *bnode_out, size_t bcap,
                                 char *err, unsigned long cap) {
  if (!ntfs_target || !ntfs_target[0]) {
    set_err(err, cap, "empty NTFS target path");
    return NULL;
  }
  if (rufux_macos_is_boot_disk(ntfs_target)) {
    set_err(err, cap, "refusing boot/system disk '%s'", ntfs_target);
    return NULL;
  }

  rufux_macos_block_node(ntfs_target, bnode_out, bcap);
  if (rufux_macos_is_boot_disk(bnode_out)) {
    set_err(err, cap, "refusing boot/system disk '%s'", ntfs_target);
    return NULL;
  }

  struct stat st;
  if (stat(bnode_out, &st) != 0) {
    set_err(err, cap, "NTFS target '%s' does not exist", ntfs_target);
    return NULL;
  }
  if (!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode) && !S_ISCHR(st.st_mode)) {
    set_err(err, cap, "NTFS target '%s' is not a block device or image file", ntfs_target);
    return NULL;
  }

  int is_dev = S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode);
  if (is_dev) {
    unmount_if_mounted_ntfs_target(bnode_out);
  }

  errno = 0;
  ntfs_volume *vol = ntfs_mount(bnode_out, 0);
  if (!vol && is_dev && (errno == EBUSY || errno == EACCES)) {
    char out[512];
    const char *av[] = {"/usr/sbin/diskutil", "unmount", "force", bnode_out, NULL};
    (void)rufux_capture(av, out, sizeof out);
    usleep(100000);
    errno = 0;
    vol = ntfs_mount(bnode_out, 0);
  }
  if (!vol) {
    set_err(err, cap, "ntfs_mount('%s') failed: %s", bnode_out, strerror(errno));
    return NULL;
  }

  // Enable case-insensitive directory lookup using vol->upcase so dual-tree
  // extractions ("EFI/Boot" + "efi/boot") merge cleanly without EEXIST/duplicates.
  if (ntfs_set_ignore_case(vol) != 0) {
    int e = errno;
    (void)ntfs_umount(vol, FALSE);
    set_err(err, cap, "ntfs_set_ignore_case('%s') failed: %s", bnode_out, strerror(e));
    return NULL;
  }
  (void)ntfs_volume_get_free_space(vol);
  return vol;
}

static int close_ntfs_rw(ntfs_volume *vol, const char *bnode, int had_error,
                         char *err, unsigned long cap) {
  int um_rc = ntfs_umount(vol, FALSE);
  if (um_rc != 0) {
    if (!had_error) {
      set_err(err, cap, "ntfs_umount('%s') failed: %s", bnode, strerror(errno));
    }
    return -1;
  }
  if (had_error) return -1;

  if (!rufux_have("ntfsfix")) {
    set_err(err, cap, "ntfsfix missing");
    return -1;
  }
  char fix_out[1024] = {0};
  const char *fix_av[] = {"ntfsfix", "-d", bnode, NULL};
  if (rufux_capture(fix_av, fix_out, sizeof fix_out) != 0) {
    set_err(err, cap, "ntfsfix -d failed on '%s': %s", bnode, fix_out);
    return -1;
  }
  return 0;
}

static int utf8_to_ntfs_component(const char *name, ntfschar **uname_out, int *ulen_out,
                                  char *err, unsigned long cap) {
  *uname_out = NULL;
  *ulen_out = 0;
  if (!name || !name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
      strchr(name, '/') != NULL) {
    set_err(err, cap, "invalid NTFS path component '%s'", name ? name : "");
    return -1;
  }

  ntfschar *u = NULL;
  int len = ntfs_mbstoucs(name, &u);
  if (len <= 0 || !u) {
    free(u);
    set_err(err, cap, "UTF-8 to UTF-16 conversion failed for '%s'", name);
    return -1;
  }
  if (len > 255) {
    free(u);
    set_err(err, cap, "NTFS filename '%s' exceeds 255 UTF-16 chars", name);
    return -1;
  }
  *uname_out = u;
  *ulen_out = len;
  return 0;
}

static ntfs_inode *ensure_ntfs_child_dir(ntfs_inode *parent_ni, const char *name,
                                         char *err, unsigned long cap) {
  ntfschar *uname = NULL;
  int ulen = 0;
  if (utf8_to_ntfs_component(name, &uname, &ulen, err, cap) != 0) return NULL;

  errno = 0;
  u64 mref = ntfs_inode_lookup_by_name(parent_ni, uname, ulen);
  if (mref != (u64)-1) {
    free(uname);
    ntfs_inode *child_ni = ntfs_inode_open(parent_ni->vol, MREF(mref));
    if (!child_ni) {
      set_err(err, cap, "ntfs_inode_open dir '%s' failed: %s", name, strerror(errno));
      return NULL;
    }
    if ((child_ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) == 0) {
      (void)ntfs_inode_close_in_dir(child_ni, parent_ni);
      set_err(err, cap, "existing NTFS entry '%s' is not a directory", name);
      return NULL;
    }
    return child_ni;
  }

  if (errno != ENOENT) {
    int e = errno;
    free(uname);
    set_err(err, cap, "ntfs_inode_lookup_by_name '%s' failed: %s", name, strerror(e));
    return NULL;
  }

  ntfs_inode *child_ni = ntfs_create(parent_ni, const_cpu_to_le32(0),
                                     uname, (u8)ulen, S_IFDIR);
  int e = errno;
  free(uname);
  if (!child_ni) {
    set_err(err, cap, "ntfs_create dir '%s' failed: %s", name, strerror(e));
    return NULL;
  }
  return child_ni;
}

static int write_regular_file_into_dir(ntfs_inode *dir_ni, const char *leaf_name,
                                       const char *host_file, const struct stat *st,
                                       NtfsCopyCtx *ctx, char *err, unsigned long cap) {
  ntfschar *uname = NULL;
  int ulen = 0;
  if (utf8_to_ntfs_component(leaf_name, &uname, &ulen, err, cap) != 0) return -1;

  int fd = open(host_file, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    int e = errno;
    free(uname);
    set_err(err, cap, "open '%s' failed: %s", host_file, strerror(e));
    return -1;
  }

  ntfs_inode *ni = NULL;
  errno = 0;
  u64 mref = ntfs_inode_lookup_by_name(dir_ni, uname, ulen);
  if (mref != (u64)-1) {
    free(uname);
    uname = NULL;
    ni = ntfs_inode_open(dir_ni->vol, MREF(mref));
    if (!ni) {
      int e = errno;
      close(fd);
      set_err(err, cap, "ntfs_inode_open '%s' failed: %s", leaf_name, strerror(e));
      return -1;
    }
    if ((ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) != 0) {
      (void)ntfs_inode_close_in_dir(ni, dir_ni);
      close(fd);
      set_err(err, cap, "cannot overwrite NTFS directory '%s' with file", leaf_name);
      return -1;
    }
    if ((ni->flags & FILE_ATTR_REPARSE_POINT) != 0) {
      (void)ntfs_inode_close_in_dir(ni, dir_ni);
      close(fd);
      set_err(err, cap, "cannot overwrite NTFS symlink/reparse '%s' with file", leaf_name);
      return -1;
    }
  } else if (errno != ENOENT) {
    int e = errno;
    free(uname);
    close(fd);
    set_err(err, cap, "ntfs_inode_lookup_by_name '%s' failed: %s", leaf_name, strerror(e));
    return -1;
  } else {
    ni = ntfs_create(dir_ni, const_cpu_to_le32(0), uname, (u8)ulen, S_IFREG);
    int e = errno;
    free(uname);
    uname = NULL;
    if (!ni) {
      close(fd);
      set_err(err, cap, "ntfs_create file '%s' failed: %s", leaf_name, strerror(e));
      return -1;
    }
  }

  ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
  if (!na) {
    int e = errno;
    (void)ntfs_inode_close_in_dir(ni, dir_ni);
    close(fd);
    set_err(err, cap, "ntfs_attr_open('%s') failed: %s", leaf_name, strerror(e));
    return -1;
  }

  if (na->data_size != 0) {
    if (ntfs_attr_truncate(na, 0) != 0) {
      int e = errno;
      ntfs_attr_close(na);
      (void)ntfs_inode_close_in_dir(ni, dir_ni);
      close(fd);
      set_err(err, cap, "ntfs_attr_truncate('%s') failed: %s", leaf_name, strerror(e));
      return -1;
    }
  }

  s64 pos = 0;
  for (;;) {
    ssize_t r = read(fd, ctx->buf, ctx->buf_cap);
    if (r < 0) {
      if (errno == EINTR) continue;
      int e = errno;
      ntfs_attr_close(na);
      (void)ntfs_inode_close_in_dir(ni, dir_ni);
      close(fd);
      set_err(err, cap, "read '%s' failed: %s", host_file, strerror(e));
      return -1;
    }
    if (r == 0) break;

    s64 off_in_buf = 0;
    s64 rem = (s64)r;
    while (rem > 0) {
      s64 w = ntfs_attr_pwrite(na, pos, rem, ctx->buf + off_in_buf);
      if (w <= 0) {
        int e = errno;
        ntfs_attr_close(na);
        (void)ntfs_inode_close_in_dir(ni, dir_ni);
        close(fd);
        set_err(err, cap, "ntfs_attr_pwrite '%s' at offset %lld failed: %s",
                leaf_name, (long long)pos, strerror(e));
        return -1;
      }
      pos += w;
      off_in_buf += w;
      rem -= w;
    }

    ctx->done_bytes += (unsigned long long)r;
    if (ctx->prog) {
      ctx->prog(ctx->done_bytes, ctx->total_bytes, ctx->user);
    }
  }

  close(fd);

  if (ntfs_attr_pclose(na) != 0) {
    int e = errno;
    ntfs_attr_close(na);
    (void)ntfs_inode_close_in_dir(ni, dir_ni);
    set_err(err, cap, "ntfs_attr_pclose('%s') failed: %s", leaf_name, strerror(e));
    return -1;
  }
  ntfs_attr_close(na);

  ntfs_time mt = timespec2ntfs(st->st_mtimespec);
  ni->last_data_change_time = mt;
  ni->last_mft_change_time = mt;
  ni->last_access_time = mt;
  NInoFileNameSetDirty(ni);
  NInoSetDirty(ni);

  if (ntfs_inode_close_in_dir(ni, dir_ni) != 0) {
    set_err(err, cap, "ntfs_inode_close_in_dir('%s') failed: %s",
            leaf_name, strerror(errno));
    return -1;
  }
  return 0;
}

static int write_symlink_into_dir(ntfs_inode *dir_ni, const char *leaf_name,
                                  const char *host_link, const struct stat *st,
                                  char *err, unsigned long cap) {
  char target_buf[PATH_MAX];
  ssize_t tlen = readlink(host_link, target_buf, sizeof target_buf - 1);
  if (tlen < 0) {
    set_err(err, cap, "readlink '%s' failed: %s", host_link, strerror(errno));
    return -1;
  }
  target_buf[tlen] = 0;

  ntfschar *uname = NULL;
  int ulen = 0;
  if (utf8_to_ntfs_component(leaf_name, &uname, &ulen, err, cap) != 0) return -1;

  errno = 0;
  u64 existing = ntfs_inode_lookup_by_name(dir_ni, uname, ulen);
  if (existing != (u64)-1) {
    free(uname);
    return 0;
  }
  if (errno != ENOENT) {
    int e = errno;
    free(uname);
    set_err(err, cap, "ntfs_inode_lookup_by_name '%s' failed: %s", leaf_name, strerror(e));
    return -1;
  }

  ntfschar *utarget = NULL;
  int utarget_len = ntfs_mbstoucs(target_buf, &utarget);
  if (utarget_len < 0 || !utarget) {
    free(uname);
    free(utarget);
    set_err(err, cap, "UTF-8 conversion failed for symlink target '%s'", target_buf);
    return -1;
  }

  ntfs_inode *lni = ntfs_create_symlink(dir_ni, const_cpu_to_le32(0),
                                        uname, (u8)ulen, utarget, utarget_len);
  int e = errno;
  free(uname);
  free(utarget);
  if (!lni) {
    set_err(err, cap, "ntfs_create_symlink '%s' failed: %s", leaf_name, strerror(e));
    return -1;
  }

  ntfs_time mt = timespec2ntfs(st->st_mtimespec);
  lni->last_data_change_time = mt;
  lni->last_mft_change_time = mt;
  lni->last_access_time = mt;
  NInoFileNameSetDirty(lni);
  NInoSetDirty(lni);

  if (ntfs_inode_close_in_dir(lni, dir_ni) != 0) {
    set_err(err, cap, "ntfs_inode_close_in_dir symlink '%s' failed: %s",
            leaf_name, strerror(errno));
    return -1;
  }
  return 0;
}

static int cmp_str_ptr(const void *a, const void *b) {
  const char *sa = *(const char *const *)a;
  const char *sb = *(const char *const *)b;
  return strcmp(sa, sb);
}

static int list_sorted_dir_entries(const char *host_dir, char ***names_out,
                                   size_t *count_out, char *err, unsigned long cap) {
  *names_out = NULL;
  *count_out = 0;

  DIR *d = opendir(host_dir);
  if (!d) {
    set_err(err, cap, "opendir '%s' failed: %s", host_dir, strerror(errno));
    return -1;
  }

  char **names = NULL;
  size_t count = 0, alloc = 0;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
    if (count == alloc) {
      size_t nalloc = alloc ? alloc * 2u : 16u;
      char **tmp = realloc(names, nalloc * sizeof *tmp);
      if (!tmp) {
        closedir(d);
        for (size_t i = 0; i < count; i++) free(names[i]);
        free(names);
        set_err(err, cap, "out of memory reading directory '%s'", host_dir);
        return -1;
      }
      names = tmp;
      alloc = nalloc;
    }
    names[count] = strdup(de->d_name);
    if (!names[count]) {
      closedir(d);
      for (size_t i = 0; i < count; i++) free(names[i]);
      free(names);
      set_err(err, cap, "out of memory duplicating filename");
      return -1;
    }
    count++;
  }
  closedir(d);

  if (count > 1) {
    qsort(names, count, sizeof *names, cmp_str_ptr);
  }
  *names_out = names;
  *count_out = count;
  return 0;
}

static void free_dir_entries(char **names, size_t count) {
  if (!names) return;
  for (size_t i = 0; i < count; i++) free(names[i]);
  free(names);
}

static int prescan_dir_bytes(const char *host_dir, unsigned long long *total,
                             char *err, unsigned long cap) {
  char **names = NULL;
  size_t count = 0;
  if (list_sorted_dir_entries(host_dir, &names, &count, err, cap) != 0) return -1;

  for (size_t i = 0; i < count; i++) {
    char child[PATH_MAX];
    int cn = snprintf(child, sizeof child, "%s/%s", host_dir, names[i]);
    if (cn < 0 || (size_t)cn >= sizeof child) {
      free_dir_entries(names, count);
      set_err(err, cap, "source path too long under '%s'", host_dir);
      return -1;
    }
    struct stat st;
    if (lstat(child, &st) != 0) {
      int e = errno;
      free_dir_entries(names, count);
      set_err(err, cap, "lstat '%s' failed: %s", child, strerror(e));
      return -1;
    }
    if (S_ISDIR(st.st_mode)) {
      if (prescan_dir_bytes(child, total, err, cap) != 0) {
        free_dir_entries(names, count);
        return -1;
      }
    } else if (S_ISREG(st.st_mode) && st.st_size > 0) {
      *total += (unsigned long long)st.st_size;
    }
  }
  free_dir_entries(names, count);
  return 0;
}

static int populate_dir_rec(ntfs_inode *ntfs_dir, const char *host_dir,
                            NtfsCopyCtx *ctx, char *err, unsigned long cap) {
  char **names = NULL;
  size_t count = 0;
  if (list_sorted_dir_entries(host_dir, &names, &count, err, cap) != 0) return -1;

  for (size_t i = 0; i < count; i++) {
    char child[PATH_MAX];
    int cn = snprintf(child, sizeof child, "%s/%s", host_dir, names[i]);
    if (cn < 0 || (size_t)cn >= sizeof child) {
      free_dir_entries(names, count);
      set_err(err, cap, "source path too long under '%s'", host_dir);
      return -1;
    }

    struct stat st;
    if (lstat(child, &st) != 0) {
      int e = errno;
      free_dir_entries(names, count);
      set_err(err, cap, "lstat '%s' failed: %s", child, strerror(e));
      return -1;
    }

    if (S_ISDIR(st.st_mode)) {
      ntfs_inode *sub_ni = ensure_ntfs_child_dir(ntfs_dir, names[i], err, cap);
      if (!sub_ni) {
        free_dir_entries(names, count);
        return -1;
      }
      int sub_rc = populate_dir_rec(sub_ni, child, ctx, err, cap);
      if (ntfs_inode_close_in_dir(sub_ni, ntfs_dir) != 0 && sub_rc == 0) {
        set_err(err, cap, "ntfs_inode_close_in_dir dir '%s' failed: %s",
                names[i], strerror(errno));
        sub_rc = -1;
      }
      if (sub_rc != 0) {
        free_dir_entries(names, count);
        return -1;
      }
    } else if (S_ISREG(st.st_mode)) {
      if (write_regular_file_into_dir(ntfs_dir, names[i], child, &st,
                                      ctx, err, cap) != 0) {
        free_dir_entries(names, count);
        return -1;
      }
    } else if (S_ISLNK(st.st_mode)) {
      if (write_symlink_into_dir(ntfs_dir, names[i], child, &st, err, cap) != 0) {
        free_dir_entries(names, count);
        return -1;
      }
    }
  }

  free_dir_entries(names, count);
  return 0;
}

int rufux_ntfs_populate_dir(const char *ntfs_target, const char *src_dir,
                            RufuxNtfsProgress prog, void *user,
                            char *err, unsigned long cap) {
  if (err && cap > 0) err[0] = 0;
  if (!ntfs_target || !ntfs_target[0]) {
    set_err(err, cap, "empty NTFS target path");
    return -1;
  }
  if (rufux_macos_is_boot_disk(ntfs_target)) {
    set_err(err, cap, "refusing boot/system disk '%s'", ntfs_target);
    return -1;
  }
  struct stat sst;
  if (!src_dir || !src_dir[0] || stat(src_dir, &sst) != 0 || !S_ISDIR(sst.st_mode)) {
    set_err(err, cap, "source directory '%s' missing", src_dir ? src_dir : "");
    return -1;
  }

  unsigned long long total_bytes = 0ULL;
  if (prescan_dir_bytes(src_dir, &total_bytes, err, cap) != 0) return -1;

  unsigned char *buf = malloc(NTFS_STREAM_CHUNK_BYTES);
  if (!buf) {
    set_err(err, cap, "out of memory allocating 1 MiB NTFS stream buffer");
    return -1;
  }

  char bnode[PATH_MAX];
  ntfs_volume *vol = open_ntfs_rw(ntfs_target, bnode, sizeof bnode, err, cap);
  if (!vol) {
    free(buf);
    return -1;
  }

  ntfs_inode *root_ni = ntfs_inode_open(vol, FILE_root);
  if (!root_ni) {
    set_err(err, cap, "ntfs_inode_open(FILE_root) failed: %s", strerror(errno));
    (void)close_ntfs_rw(vol, bnode, /*had_error=*/1, err, cap);
    free(buf);
    return -1;
  }

  NtfsCopyCtx ctx = {
    .buf = buf,
    .buf_cap = NTFS_STREAM_CHUNK_BYTES,
    .done_bytes = 0ULL,
    .total_bytes = total_bytes,
    .prog = prog,
    .user = user
  };
  if (prog) {
    prog(0ULL, total_bytes, user);
  }

  int rc = populate_dir_rec(root_ni, src_dir, &ctx, err, cap);

  if (ntfs_inode_close(root_ni) != 0 && rc == 0) {
    set_err(err, cap, "ntfs_inode_close(FILE_root) failed: %s", strerror(errno));
    rc = -1;
  }

  int close_rc = close_ntfs_rw(vol, bnode, /*had_error=*/(rc != 0), err, cap);
  free(buf);
  return (rc == 0 && close_rc == 0) ? 0 : -1;
}

int rufux_ntfs_write_single_file(const char *ntfs_target, const char *src_file,
                                 const char *ntfs_relpath,
                                 RufuxNtfsProgress prog, void *user,
                                 char *err, unsigned long cap) {
  if (err && cap > 0) err[0] = 0;
  if (!ntfs_target || !ntfs_target[0]) {
    set_err(err, cap, "empty NTFS target path");
    return -1;
  }
  if (rufux_macos_is_boot_disk(ntfs_target)) {
    set_err(err, cap, "refusing boot/system disk '%s'", ntfs_target);
    return -1;
  }
  struct stat sst;
  if (!src_file || !src_file[0] || stat(src_file, &sst) != 0 || !S_ISREG(sst.st_mode)) {
    set_err(err, cap, "source file '%s' missing or not a regular file",
            src_file ? src_file : "");
    return -1;
  }
  if (!ntfs_relpath || !ntfs_relpath[0]) {
    set_err(err, cap, "empty NTFS destination relative path");
    return -1;
  }

  char rel_copy[PATH_MAX];
  int rn = snprintf(rel_copy, sizeof rel_copy, "%s", ntfs_relpath);
  if (rn < 0 || (size_t)rn >= sizeof rel_copy) {
    set_err(err, cap, "NTFS destination relative path too long");
    return -1;
  }

  const char *comps[NTFS_MAX_PATH_DEPTH];
  int ncomps = 0;
  char *p = rel_copy;
  while (*p) {
    while (*p == '/' || *p == '\\') p++;
    if (!*p) break;
    if (ncomps >= NTFS_MAX_PATH_DEPTH) {
      set_err(err, cap, "NTFS path '%s' exceeds max depth", ntfs_relpath);
      return -1;
    }
    char *start = p;
    while (*p && *p != '/' && *p != '\\') p++;
    if (*p) *p++ = '\0';
    if (strcmp(start, ".") == 0 || strcmp(start, "..") == 0) {
      set_err(err, cap, "invalid relative component '%s' in '%s'", start, ntfs_relpath);
      return -1;
    }
    comps[ncomps++] = start;
  }
  if (ncomps == 0) {
    set_err(err, cap, "empty NTFS destination filename in '%s'", ntfs_relpath);
    return -1;
  }

  unsigned char *buf = malloc(NTFS_STREAM_CHUNK_BYTES);
  if (!buf) {
    set_err(err, cap, "out of memory allocating 1 MiB NTFS stream buffer");
    return -1;
  }

  char bnode[PATH_MAX];
  ntfs_volume *vol = open_ntfs_rw(ntfs_target, bnode, sizeof bnode, err, cap);
  if (!vol) {
    free(buf);
    return -1;
  }

  ntfs_inode *dir_stack[NTFS_MAX_PATH_DEPTH + 1];
  int top = 0;
  dir_stack[0] = ntfs_inode_open(vol, FILE_root);
  if (!dir_stack[0]) {
    set_err(err, cap, "ntfs_inode_open(FILE_root) failed: %s", strerror(errno));
    (void)close_ntfs_rw(vol, bnode, /*had_error=*/1, err, cap);
    free(buf);
    return -1;
  }

  int rc = 0;
  for (int d = 0; d < ncomps - 1; d++) {
    ntfs_inode *child_dir = ensure_ntfs_child_dir(dir_stack[top], comps[d], err, cap);
    if (!child_dir) {
      rc = -1;
      break;
    }
    dir_stack[++top] = child_dir;
  }

  if (rc == 0) {
    unsigned long long total_bytes = (unsigned long long)sst.st_size;
    NtfsCopyCtx ctx = {
      .buf = buf,
      .buf_cap = NTFS_STREAM_CHUNK_BYTES,
      .done_bytes = 0ULL,
      .total_bytes = total_bytes,
      .prog = prog,
      .user = user
    };
    if (prog) {
      prog(0ULL, total_bytes, user);
    }

    rc = write_regular_file_into_dir(dir_stack[top], comps[ncomps - 1],
                                     src_file, &sst, &ctx, err, cap);
  }

  // Close open directories strictly from deepest child up to FILE_root so
  // ntfs_inode_close_in_dir syncs each directory entry into its live parent.
  for (int k = top; k >= 1; k--) {
    if (ntfs_inode_close_in_dir(dir_stack[k], dir_stack[k - 1]) != 0 && rc == 0) {
      set_err(err, cap, "ntfs_inode_close_in_dir failed: %s", strerror(errno));
      rc = -1;
    }
  }
  if (ntfs_inode_close(dir_stack[0]) != 0 && rc == 0) {
    set_err(err, cap, "ntfs_inode_close(FILE_root) failed: %s", strerror(errno));
    rc = -1;
  }

  int close_rc = close_ntfs_rw(vol, bnode, /*had_error=*/(rc != 0), err, cap);
  free(buf);
  return (rc == 0 && close_rc == 0) ? 0 : -1;
}

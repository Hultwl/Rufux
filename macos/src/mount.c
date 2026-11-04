#include "mount.h"
#include "exec.h"
#include "platform_macos.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <ntfs-3g/volume.h>

__attribute__((format(printf, 3, 4)))
static void set_err(char *err, unsigned long cap, const char *fmt, ...) {
  if (!err || cap == 0) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, cap, fmt, ap);
  va_end(ap);
}

static uint64_t fnv1a64(const char *s) {
  uint64_t h = 14695981039346656037ULL;
  if (!s) return h;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    h ^= (uint64_t)(*p);
    h *= 1099511628211ULL;
  }
  return h;
}

void rufux_part1(const char *disk, char *out, unsigned long cap) {
  if (!out || cap == 0) return;
  if (!disk || !disk[0]) {
    out[0] = '\0';
    return;
  }
  char bnode[PATH_MAX];
  rufux_macos_block_node(disk, bnode, sizeof(bnode));
  rufux_macos_part_slice(bnode, 1, out, (size_t)cap);
}

static int parse_bsd_ident(const char *path, char *bsd_out, size_t bcap) {
  if (bsd_out && bcap > 0) bsd_out[0] = '\0';
  if (!path || !path[0]) return 0;

  char resolved[PATH_MAX];
  const char *p = realpath(path, resolved) ? resolved : path;
  if (strncmp(p, "/private/dev/", 13) == 0) p += 8;

  const char *cand = NULL;
  if (strncmp(p, "/dev/rdisk", 10) == 0) cand = p + 6;
  else if (strncmp(p, "/dev/disk", 9) == 0) cand = p + 5;
  else if (strncmp(p, "disk", 4) == 0) cand = p;
  else return 0;

  if (strncmp(cand, "disk", 4) != 0 || !isdigit((unsigned char)cand[4])) return 0;
  snprintf(bsd_out, bcap, "%s", cand);
  return 1;
}

static int is_whole_disk_bsd_ident(const char *bsd) {
  if (!bsd || strncmp(bsd, "disk", 4) != 0 || !isdigit((unsigned char)bsd[4])) return 0;
  for (size_t i = 4; bsd[i]; i++) {
    if (!isdigit((unsigned char)bsd[i])) return 0;
  }
  return 1;
}

static int bsd_belongs_to_whole_ident(const char *bsd, const char *whole) {
  if (!bsd || !whole || !is_whole_disk_bsd_ident(whole)) return 0;
  size_t wlen = strlen(whole);
  if (strncmp(bsd, whole, wlen) != 0) return 0;
  if (bsd[wlen] == '\0') return 1;
  return (bsd[wlen] == 's' && isdigit((unsigned char)bsd[wlen + 1])) ? 1 : 0;
}

static int find_active_mount_for_dev(const char *dev_path, char *mnt_out,
                                     size_t mcap, int *rdonly_out,
                                     char *vfs_out, size_t vcap) {
  if (mnt_out && mcap > 0) mnt_out[0] = '\0';
  if (rdonly_out) *rdonly_out = 0;
  if (vfs_out && vcap > 0) vfs_out[0] = '\0';

  char target_bsd[64];
  int have_bsd = parse_bsd_ident(dev_path, target_bsd, sizeof(target_bsd));

  struct statfs *mnts = NULL;
  int n = getmntinfo(&mnts, MNT_NOWAIT);
  if (n <= 0 || !mnts) return 0;

  for (int i = 0; i < n; i++) {
    int match = 0;
    if (have_bsd) {
      char mbsd[64];
      if (parse_bsd_ident(mnts[i].f_mntfromname, mbsd, sizeof(mbsd)) &&
          strcmp(mbsd, target_bsd) == 0) {
        match = 1;
      }
    } else if (strcmp(mnts[i].f_mntfromname, dev_path) == 0) {
      match = 1;
    }
    if (match) {
      if (mnt_out && mcap > 0) snprintf(mnt_out, mcap, "%s", mnts[i].f_mntonname);
      if (rdonly_out) *rdonly_out = ((mnts[i].f_flags & MNT_RDONLY) != 0);
      if (vfs_out && vcap > 0) snprintf(vfs_out, vcap, "%s", mnts[i].f_fstypename);
      return 1;
    }
  }
  return 0;
}

static void unmount_existing_os_mount(const char *bnode) {
  char cur_mnt[PATH_MAX];
  int rdonly = 0;
  if (!find_active_mount_for_dev(bnode, cur_mnt, sizeof(cur_mnt), &rdonly, NULL, 0)) {
    return;
  }
  char out[512];
  const char *av1[] = {"/usr/sbin/diskutil", "unmount", "force", bnode, NULL};
  (void)rufux_capture(av1, out, sizeof(out));
  if (find_active_mount_for_dev(bnode, cur_mnt, sizeof(cur_mnt), &rdonly, NULL, 0)) {
    const char *av2[] = {"/sbin/umount", "-f", cur_mnt, NULL};
    (void)rufux_capture(av2, out, sizeof(out));
  }
  usleep(100000);
}

static int resolve_state_dir(char *out, size_t cap) {
  const char *env = getenv("RUFUX_MOUNT_STATE_DIR");
  if (env && env[0]) {
    snprintf(out, cap, "%s", env);
  } else {
    snprintf(out, cap, "/tmp/rufux-mount-state-%u", (unsigned)geteuid());
  }
  if (mkdir(out, 0700) != 0 && errno != EEXIST) return -1;
  return 0;
}

static int resolve_mountd_bin(char *out, size_t cap) {
  rufux_macos_init_bundle_env();
  const char *exedir = rufux_macos_exe_dir();
  if (exedir && exedir[0]) {
    int n = snprintf(out, cap, "%s/rufux-mountd", exedir);
    if (n > 0 && (size_t)n < cap && access(out, X_OK) == 0) return 0;

    n = snprintf(out, cap, "%s/../Resources/bin/rufux-mountd", exedir);
    if (n > 0 && (size_t)n < cap && access(out, X_OK) == 0) return 0;
  }
  const char *resolved = rufux_resolve_tool("rufux-mountd");
  if (resolved && resolved[0] && access(resolved, X_OK) == 0) {
    snprintf(out, cap, "%s", resolved);
    return 0;
  }
  return -1;
}

static void sanitize_label(const char *raw, const char *fallback_path,
                           char *out, size_t cap) {
  char tmp[128];
  size_t j = 0;
  if (raw && raw[0]) {
    for (size_t i = 0; raw[i] && j + 1 < sizeof(tmp); i++) {
      unsigned char c = (unsigned char)raw[i];
      if (c == '/' || c == '\\' || c == ':' || c < 32) {
        tmp[j++] = '_';
      } else {
        tmp[j++] = (char)c;
      }
    }
  }
  tmp[j] = '\0';
  while (j > 0 && (tmp[j - 1] == ' ' || tmp[j - 1] == '.')) tmp[--j] = '\0';
  char *start = tmp;
  while (*start == ' ' || *start == '.') start++;

  if (*start) {
    snprintf(out, cap, "%s", start);
    return;
  }

  const char *base = fallback_path ? strrchr(fallback_path, '/') : NULL;
  base = base ? (base + 1) : (fallback_path ? fallback_path : "volume");
  j = 0;
  for (size_t i = 0; base[i] && j + 1 < sizeof(tmp); i++) {
    if (strcmp(base + i, ".img") == 0) break;
    unsigned char c = (unsigned char)base[i];
    tmp[j++] = (c == '/' || c < 32) ? '_' : (char)c;
  }
  tmp[j] = '\0';
  snprintf(out, cap, "%s", tmp[0] ? tmp : "rufux-vol");
}

static int detect_fs_magic_and_label(const char *bnode, int is_dev,
                                     const char *hint_label,
                                     char *fstype_out, size_t fcap,
                                     char *label_out, size_t lcap) {
  fstype_out[0] = '\0';
  label_out[0] = '\0';

  int fd = open(bnode, O_RDONLY | O_CLOEXEC);
  if (fd < 0 && is_dev && errno == EBUSY) {
    unmount_existing_os_mount(bnode);
    fd = open(bnode, O_RDONLY | O_CLOEXEC);
  }
  if (fd < 0) return -1;

  uint8_t buf[4096];
  memset(buf, 0, sizeof(buf));
  ssize_t r = pread(fd, buf, sizeof(buf), 0);
  close(fd);
  if (r < 512) return -1;

  if (memcmp(buf + 3, "NTFS    ", 8) == 0) {
    snprintf(fstype_out, fcap, "ntfs");
    char raw_label[128] = {0};
    if (hint_label && hint_label[0]) {
      snprintf(raw_label, sizeof(raw_label), "%s", hint_label);
    } else {
      ntfs_volume *v = ntfs_mount(bnode, NTFS_MNT_RDONLY);
      if (v) {
        if (v->vol_name && v->vol_name[0]) {
          snprintf(raw_label, sizeof(raw_label), "%s", v->vol_name);
        }
        (void)ntfs_umount(v, FALSE);
      }
    }
    sanitize_label(raw_label, bnode, label_out, lcap);
    return 0;
  }

  if (r >= 1160 && buf[1080] == 0x53U && buf[1081] == 0xEFU) {
    snprintf(fstype_out, fcap, "ext");
    char raw_label[17];
    memcpy(raw_label, buf + 1144, 16);
    raw_label[16] = '\0';
    sanitize_label(raw_label, bnode, label_out, lcap);
    return 0;
  }

  snprintf(fstype_out, fcap, "native");
  sanitize_label(hint_label, bnode, label_out, lcap);
  return 0;
}

static int make_unique_mount_dir(const char *label, char *mnt_out, size_t cap) {
  char root[PATH_MAX];
  const char *prefix = "";
  const char *env_root = getenv("RUFUX_MOUNT_ROOT");
  if (env_root && env_root[0]) {
    snprintf(root, sizeof(root), "%s", env_root);
    (void)mkdir(root, 0755);
  } else if (access("/Volumes", W_OK | X_OK) == 0) {
    snprintf(root, sizeof(root), "/Volumes");
  } else {
    snprintf(root, sizeof(root), "/tmp");
    prefix = "rufux-mnt-";
  }

  for (int suffix = 0; suffix < 1000; suffix++) {
    int n;
    if (suffix == 0) {
      n = snprintf(mnt_out, cap, "%s/%s%s", root, prefix, label);
    } else {
      n = snprintf(mnt_out, cap, "%s/%s%s-%d", root, prefix, label, suffix);
    }
    if (n < 0 || (size_t)n >= cap) return -1;
    if (mkdir(mnt_out, 0755) == 0) return 0;
    if (errno != EEXIST) return -1;
  }
  return -1;
}

typedef struct {
  char state_file[PATH_MAX];
  char kind[32];
  pid_t pid;
  char dev[PATH_MAX];
  char canon_dev[PATH_MAX];
  char mnt[PATH_MAX];
  char canon_mnt[PATH_MAX];
  char attached_disk[128];
  char pidfile[PATH_MAX];
  int created_dir;
} MountState;

static int write_mount_state(const char *state_dir, const MountState *st) {
  uint64_t id = fnv1a64(st->canon_dev[0] ? st->canon_dev : st->dev) ^
                (fnv1a64(st->mnt) << 1);
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/%016llx.state",
           state_dir, (unsigned long long)id);
  FILE *f = fopen(path, "w");
  if (!f) return -1;
  fprintf(f, "kind=%s\n", st->kind);
  fprintf(f, "pid=%ld\n", (long)st->pid);
  fprintf(f, "dev=%s\n", st->dev);
  fprintf(f, "canon_dev=%s\n", st->canon_dev);
  fprintf(f, "mnt=%s\n", st->mnt);
  fprintf(f, "canon_mnt=%s\n", st->canon_mnt);
  fprintf(f, "attached_disk=%s\n", st->attached_disk);
  fprintf(f, "pidfile=%s\n", st->pidfile);
  fprintf(f, "created_dir=%d\n", st->created_dir);
  fclose(f);
  return 0;
}

static void strip_trailing_slashes(char *s) {
  if (!s) return;
  size_t n = strlen(s);
  while (n > 1 && s[n - 1] == '/') {
    s[--n] = '\0';
  }
}

static int is_mountpoint_active(const char *mnt, const char *canon_mnt) {
  if ((!mnt || !mnt[0]) && (!canon_mnt || !canon_mnt[0])) return 0;

  char raw_m[PATH_MAX] = {0};
  char can_m[PATH_MAX] = {0};
  char res_m[PATH_MAX] = {0};
  if (mnt && mnt[0]) {
    snprintf(raw_m, sizeof(raw_m), "%s", mnt);
    strip_trailing_slashes(raw_m);
    if (realpath(raw_m, res_m)) strip_trailing_slashes(res_m);
  }
  if (canon_mnt && canon_mnt[0]) {
    snprintf(can_m, sizeof(can_m), "%s", canon_mnt);
    strip_trailing_slashes(can_m);
  }

  struct statfs *mnts = NULL;
  int n = getmntinfo(&mnts, MNT_NOWAIT);
  if (n <= 0 || !mnts) return 0;

  for (int i = 0; i < n; i++) {
    char on[PATH_MAX];
    snprintf(on, sizeof(on), "%s", mnts[i].f_mntonname);
    strip_trailing_slashes(on);
    if ((raw_m[0] && strcmp(on, raw_m) == 0) ||
        (can_m[0] && strcmp(on, can_m) == 0) ||
        (res_m[0] && strcmp(on, res_m) == 0)) {
      return 1;
    }
    char on_res[PATH_MAX];
    if (realpath(on, on_res)) {
      strip_trailing_slashes(on_res);
      if ((raw_m[0] && strcmp(on_res, raw_m) == 0) ||
          (can_m[0] && strcmp(on_res, can_m) == 0) ||
          (res_m[0] && strcmp(on_res, res_m) == 0)) {
        return 1;
      }
    }
  }
  return 0;
}

static int read_mount_state_file(const char *path, MountState *st) {
  memset(st, 0, sizeof(*st));
  snprintf(st->state_file, sizeof(st->state_file), "%s", path);
  FILE *f = fopen(path, "r");
  if (!f) return -1;

  char line[PATH_MAX + 64];
  while (fgets(line, sizeof(line), f)) {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = '\0';
    const char *k = line;
    const char *v = eq + 1;
    if (strcmp(k, "kind") == 0) snprintf(st->kind, sizeof(st->kind), "%s", v);
    else if (strcmp(k, "pid") == 0) st->pid = (pid_t)atol(v);
    else if (strcmp(k, "dev") == 0) snprintf(st->dev, sizeof(st->dev), "%s", v);
    else if (strcmp(k, "canon_dev") == 0) snprintf(st->canon_dev, sizeof(st->canon_dev), "%s", v);
    else if (strcmp(k, "mnt") == 0) snprintf(st->mnt, sizeof(st->mnt), "%s", v);
    else if (strcmp(k, "canon_mnt") == 0) snprintf(st->canon_mnt, sizeof(st->canon_mnt), "%s", v);
    else if (strcmp(k, "attached_disk") == 0) snprintf(st->attached_disk, sizeof(st->attached_disk), "%s", v);
    else if (strcmp(k, "pidfile") == 0) snprintf(st->pidfile, sizeof(st->pidfile), "%s", v);
    else if (strcmp(k, "created_dir") == 0) st->created_dir = atoi(v);
  }
  fclose(f);
  return st->kind[0] ? 0 : -1;
}

static int state_matches_target(const MountState *st, const char *raw_t,
                                const char *blk_t, const char *canon_t) {
  const char *cands[3] = {raw_t, blk_t, canon_t};
  for (int i = 0; i < 3; i++) {
    const char *c = cands[i];
    if (!c || !c[0]) continue;
    if ((st->dev[0] && strcmp(st->dev, c) == 0) ||
        (st->canon_dev[0] && strcmp(st->canon_dev, c) == 0) ||
        (st->mnt[0] && strcmp(st->mnt, c) == 0) ||
        (st->canon_mnt[0] && strcmp(st->canon_mnt, c) == 0) ||
        (st->attached_disk[0] && strcmp(st->attached_disk, c) == 0)) {
      return 1;
    }
  }
  char tbsd[64];
  if (parse_bsd_ident(blk_t, tbsd, sizeof(tbsd)) && is_whole_disk_bsd_ident(tbsd)) {
    char sbsd[64];
    if ((parse_bsd_ident(st->canon_dev, sbsd, sizeof(sbsd)) &&
         bsd_belongs_to_whole_ident(sbsd, tbsd)) ||
        (parse_bsd_ident(st->dev, sbsd, sizeof(sbsd)) &&
         bsd_belongs_to_whole_ident(sbsd, tbsd)) ||
        (parse_bsd_ident(st->attached_disk, sbsd, sizeof(sbsd)) &&
         bsd_belongs_to_whole_ident(sbsd, tbsd))) {
      return 1;
    }
  }
  return 0;
}

static int find_matching_state(const char *state_dir, const char *target,
                               MountState *out) {
  char raw_t[PATH_MAX], blk_t[PATH_MAX], canon_t[PATH_MAX];
  snprintf(raw_t, sizeof(raw_t), "%s", target);
  strip_trailing_slashes(raw_t);
  rufux_macos_block_node(raw_t, blk_t, sizeof(blk_t));
  strip_trailing_slashes(blk_t);
  if (!realpath(blk_t, canon_t)) {
    if (!realpath(raw_t, canon_t)) canon_t[0] = '\0';
  }
  strip_trailing_slashes(canon_t);

  DIR *d = opendir(state_dir);
  if (!d) return -1;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    size_t len = strlen(de->d_name);
    if (len < 7 || strcmp(de->d_name + len - 6, ".state") != 0) continue;
    char spath[PATH_MAX];
    snprintf(spath, sizeof(spath), "%s/%s", state_dir, de->d_name);
    MountState cur;
    if (read_mount_state_file(spath, &cur) == 0 &&
        state_matches_target(&cur, raw_t, blk_t, canon_t)) {
      *out = cur;
      closedir(d);
      return 0;
    }
  }
  closedir(d);
  return -1;
}

static int pid_is_alive(pid_t pid) {
  if (pid <= 1) return 0;
  if (kill(pid, 0) == 0) return 1;
  return (errno == EPERM) ? 1 : 0;
}

static void terminate_and_wait_pid(pid_t pid, int timeout_ms) {
  if (pid <= 1) return;
  if (kill(pid, SIGTERM) != 0 && errno == ESRCH) return;

  int elapsed = 0;
  while (elapsed < timeout_ms) {
    int status = 0;
    pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) return;
    if (kill(pid, 0) != 0 && errno == ESRCH) return;
    usleep(20000);
    elapsed += 20;
  }
  (void)kill(pid, SIGKILL);
  (void)waitpid(pid, NULL, WNOHANG);
  usleep(20000);
}

static void rmdir_with_retry(const char *dir) {
  if (!dir || !dir[0] || strcmp(dir, "/") == 0 || strcmp(dir, "/Volumes") == 0) {
    return;
  }
  for (int i = 0; i < 25; i++) {
    if (rmdir(dir) == 0 || errno == ENOENT) return;
    usleep(20000);
  }
}

static int launch_mountd_and_mount_nfs(const char *mountd_bin,
                                       const char *canon_dev,
                                       const char *fstype,
                                       const char *mnt_dir,
                                       const char *pidfile,
                                       pid_t *pid_out,
                                       char *err, unsigned long errcap) {
  int ready_p[2] = {-1, -1};
  int err_p[2] = {-1, -1};
  if (pipe(ready_p) != 0) {
    set_err(err, errcap, "pipe failed: %s", strerror(errno));
    return -1;
  }
  if (pipe(err_p) != 0) {
    close(ready_p[0]);
    close(ready_p[1]);
    set_err(err, errcap, "pipe failed: %s", strerror(errno));
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    int e = errno;
    close(ready_p[0]); close(ready_p[1]);
    close(err_p[0]);   close(err_p[1]);
    set_err(err, errcap, "fork rufux-mountd failed: %s", strerror(e));
    return -1;
  }

  if (pid == 0) {
    (void)setsid();
    close(ready_p[0]);
    close(err_p[0]);
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
      (void)dup2(null_fd, STDIN_FILENO);
      (void)dup2(null_fd, STDOUT_FILENO);
      if (null_fd > STDERR_FILENO && null_fd != ready_p[1] && null_fd != err_p[1]) {
        close(null_fd);
      }
    }
    (void)dup2(err_p[1], STDERR_FILENO);
    if (err_p[1] > STDERR_FILENO && err_p[1] != ready_p[1]) close(err_p[1]);

    /* Clear FD_CLOEXEC on ready_p[1] so child rufux-mountd inherits it */
    int fdfl = fcntl(ready_p[1], F_GETFD, 0);
    if (fdfl >= 0) (void)fcntl(ready_p[1], F_SETFD, fdfl & ~FD_CLOEXEC);

    char fd_buf[32];
    snprintf(fd_buf, sizeof(fd_buf), "%d", ready_p[1]);
    char *const av[] = {
      (char *)mountd_bin,
      "--device",   (char *)canon_dev,
      "--fstype",   (char *)fstype,
      "--ready-fd", fd_buf,
      "--pidfile",  (char *)pidfile,
      NULL
    };
    execv(mountd_bin, av);
    fprintf(stderr, "execv('%s') failed: %s\n", mountd_bin, strerror(errno));
    _exit(127);
  }

  close(ready_p[1]);
  close(err_p[1]);

  char port_buf[64] = {0};
  size_t pgot = 0;
  while (pgot + 1 < sizeof(port_buf)) {
    struct pollfd pfd = {ready_p[0], POLLIN, 0};
    int pr = poll(&pfd, 1, 10000);
    if (pr <= 0) break;
    ssize_t r = read(ready_p[0], port_buf + pgot, sizeof(port_buf) - 1 - pgot);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    pgot += (size_t)r;
    if (strchr(port_buf, '\n') != NULL) break;
  }
  close(ready_p[0]);

  unsigned port = (unsigned)strtoul(port_buf, NULL, 10);
  if (port == 0 || port > 65535U) {
    char ebuf[512] = {0};
    ssize_t er = read(err_p[0], ebuf, sizeof(ebuf) - 1);
    if (er > 0) ebuf[er] = '\0';
    close(err_p[0]);
    terminate_and_wait_pid(pid, 2000);
    set_err(err, errcap, "rufux-mountd failed to start on '%s': %s",
            canon_dev, ebuf[0] ? ebuf : "no ready port reported");
    return -1;
  }
  close(err_p[0]);

  char opts[256];
  snprintf(opts, sizeof(opts),
           "locallocks,vers=3,tcp,port=%u,mountport=%u,nolockd,rsize=65536,wsize=65536",
           port, port);

  char mout[512] = {0};
  const char *mav[] = {"/sbin/mount_nfs", "-o", opts, "127.0.0.1:/", mnt_dir, NULL};
  int mrc = rufux_capture(mav, mout, sizeof(mout));
  if (mrc != 0) {
    /* Fallback if macOS mount_nfs rejects combined locallocks + nolockd */
    snprintf(opts, sizeof(opts),
             "locallocks,vers=3,tcp,port=%u,mountport=%u,rsize=65536,wsize=65536",
             port, port);
    mout[0] = '\0';
    mrc = rufux_capture(mav, mout, sizeof(mout));
  }
  if (mrc != 0) {
    terminate_and_wait_pid(pid, 3000);
    (void)unlink(pidfile);
    set_err(err, errcap, "mount_nfs failed on '%s': %s", mnt_dir, mout);
    return -1;
  }

  *pid_out = pid;
  return 0;
}

int rufux_mount(const char *dev, int dry_run, char *mnt_out, unsigned long cap,
                char *err, unsigned long errcap) {
  if (mnt_out && cap > 0) mnt_out[0] = '\0';
  if (err && errcap > 0) err[0] = '\0';

  if (!dev || !dev[0]) {
    set_err(err, errcap, "empty device/image path");
    return -1;
  }
  if (rufux_macos_is_boot_disk(dev)) {
    set_err(err, errcap, "refusing to mount boot/system disk '%s'", dev);
    return -1;
  }

  char bnode[PATH_MAX];
  rufux_macos_block_node(dev, bnode, sizeof(bnode));
  if (rufux_macos_is_boot_disk(bnode)) {
    set_err(err, errcap, "refusing to mount boot/system disk '%s'", dev);
    return -1;
  }

  if (dry_run) {
    if (mnt_out && cap > 0) snprintf(mnt_out, (size_t)cap, "/Volumes/rufux-dryrun");
    return 0;
  }

  struct stat st;
  if (stat(bnode, &st) != 0) {
    set_err(err, errcap, "target '%s' does not exist: %s", dev, strerror(errno));
    return -1;
  }
  int is_dev = S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode);
  if (!S_ISREG(st.st_mode) && !is_dev) {
    set_err(err, errcap, "target '%s' is not a block device or disk image", dev);
    return -1;
  }

  /* If caller passed a whole disk (e.g. /dev/disk5) that has slice 1 (/dev/disk5s1),
     resolve bnode to slice 1 automatically so `rufux mount /dev/disk5` mounts part 1. */
  if (is_dev) {
    char dbsd[64];
    if (parse_bsd_ident(bnode, dbsd, sizeof(dbsd)) && is_whole_disk_bsd_ident(dbsd)) {
      char part1[PATH_MAX];
      struct stat pst;
      rufux_part1(bnode, part1, sizeof(part1));
      if (part1[0] && stat(part1, &pst) == 0 &&
          (S_ISBLK(pst.st_mode) || S_ISCHR(pst.st_mode))) {
        snprintf(bnode, sizeof(bnode), "%s", part1);
      }
    }
  }

  char canon_dev[PATH_MAX];
  if (!realpath(bnode, canon_dev)) {
    snprintf(canon_dev, sizeof(canon_dev), "%s", bnode);
  }

  char state_dir[PATH_MAX];
  if (resolve_state_dir(state_dir, sizeof(state_dir)) != 0) {
    set_err(err, errcap, "failed to create mount state directory '%s'", state_dir);
    return -1;
  }

  /* Check existing tracked mount state for idempotency or external-eject cleanup */
  MountState existing;
  while (find_matching_state(state_dir, canon_dev, &existing) == 0 ||
         find_matching_state(state_dir, dev, &existing) == 0) {
    int mounted = is_mountpoint_active(existing.mnt, existing.canon_mnt);
    if (strcmp(existing.kind, "nfs") == 0) {
      int pid_alive = pid_is_alive(existing.pid);
      if (mounted && pid_alive) {
        if (mnt_out && cap > 0) snprintf(mnt_out, (size_t)cap, "%s", existing.mnt);
        return 0;
      }
      if (mounted) {
        char uout[512];
        const char *uav[] = {"/sbin/umount", "-f", existing.mnt, NULL};
        (void)rufux_capture(uav, uout, sizeof(uout));
      }
      if (is_mountpoint_active(existing.mnt, existing.canon_mnt)) {
        set_err(err, errcap,
                "stale NFS mount '%s' could not be unmounted before remount",
                existing.mnt);
        return -1;
      }
      if (existing.pid > 1) {
        terminate_and_wait_pid(existing.pid, 5000);
        if (pid_is_alive(existing.pid)) {
          set_err(err, errcap,
                  "unable to terminate orphaned rufux-mountd pid %d",
                  (int)existing.pid);
          return -1;
        }
      }
      if (existing.pidfile[0]) (void)unlink(existing.pidfile);
      if (existing.created_dir) rmdir_with_retry(existing.mnt);
      if (unlink(existing.state_file) != 0) break;
    } else {
      if (mounted) {
        if (mnt_out && cap > 0) snprintf(mnt_out, (size_t)cap, "%s", existing.mnt);
        return 0;
      }
      if (strcmp(existing.kind, "hdiutil") == 0 && existing.attached_disk[0]) {
        char hout[512];
        const char *hav[] = {"/usr/bin/hdiutil", "detach", "-force",
                             existing.attached_disk, NULL};
        (void)rufux_capture(hav, hout, sizeof(hout));
      }
      if (existing.created_dir) rmdir_with_retry(existing.mnt);
      if (unlink(existing.state_file) != 0) break;
    }
  }

  char hint_label[128] = {0};
  if (is_dev) {
    char existing_mnt[PATH_MAX] = {0};
    char active_vfs[64] = {0};
    int rdonly = 0;
    if (find_active_mount_for_dev(bnode, existing_mnt, sizeof(existing_mnt),
                                  &rdonly, active_vfs, sizeof(active_vfs))) {
      const char *mbase = strrchr(existing_mnt, '/');
      if (mbase && mbase[1]) {
        snprintf(hint_label, sizeof(hint_label), "%s", mbase + 1);
      }
      if (!rdonly && strcmp(active_vfs, "ntfs") != 0 &&
          strncmp(active_vfs, "ext", 3) != 0) {
        if (mnt_out && cap > 0) snprintf(mnt_out, (size_t)cap, "%s", existing_mnt);
        return 0;
      }
      unmount_existing_os_mount(bnode);
    }
  }

  char fstype[32], label[128];
  if (detect_fs_magic_and_label(canon_dev, is_dev, hint_label,
                                fstype, sizeof(fstype),
                                label, sizeof(label)) != 0) {
    set_err(err, errcap, "failed to probe filesystem magic on '%s'", canon_dev);
    return -1;
  }

  if (strcmp(fstype, "ntfs") == 0 || strcmp(fstype, "ext") == 0) {
    char mountd_bin[PATH_MAX];
    if (resolve_mountd_bin(mountd_bin, sizeof(mountd_bin)) != 0) {
      set_err(err, errcap, "rufux-mountd helper binary not found");
      return -1;
    }

    if (is_dev) unmount_existing_os_mount(bnode);

    char mnt_dir[PATH_MAX];
    if (make_unique_mount_dir(label, mnt_dir, sizeof(mnt_dir)) != 0) {
      set_err(err, errcap, "failed to create mountpoint directory for '%s'", label);
      return -1;
    }

    uint64_t id = fnv1a64(canon_dev) ^ (fnv1a64(mnt_dir) << 1);
    char pidfile[PATH_MAX];
    snprintf(pidfile, sizeof(pidfile), "%s/%016llx.pid",
             state_dir, (unsigned long long)id);

    pid_t pid = 0;
    int lrc = launch_mountd_and_mount_nfs(mountd_bin, canon_dev, fstype, mnt_dir,
                                          pidfile, &pid, err, errcap);
    if (lrc != 0 && is_dev) {
      unmount_existing_os_mount(bnode);
      lrc = launch_mountd_and_mount_nfs(mountd_bin, canon_dev, fstype, mnt_dir,
                                        pidfile, &pid, err, errcap);
    }
    if (lrc != 0) {
      rmdir_with_retry(mnt_dir);
      return -1;
    }

    MountState ms;
    memset(&ms, 0, sizeof(ms));
    snprintf(ms.kind, sizeof(ms.kind), "nfs");
    ms.pid = pid;
    snprintf(ms.dev, sizeof(ms.dev), "%s", dev);
    snprintf(ms.canon_dev, sizeof(ms.canon_dev), "%s", canon_dev);
    snprintf(ms.mnt, sizeof(ms.mnt), "%s", mnt_dir);
    if (!realpath(mnt_dir, ms.canon_mnt)) {
      snprintf(ms.canon_mnt, sizeof(ms.canon_mnt), "%s", mnt_dir);
    }
    snprintf(ms.pidfile, sizeof(ms.pidfile), "%s", pidfile);
    ms.created_dir = 1;
    (void)write_mount_state(state_dir, &ms);

    if (mnt_out && cap > 0) snprintf(mnt_out, (size_t)cap, "%s", mnt_dir);
    return 0;
  }

  /* Native macOS filesystem fallback (FAT12/16/32, exFAT, UDF, HFS+, APFS) */
  if (is_dev) {
    char existing_mnt[PATH_MAX];
    int rdonly = 0;
    if (find_active_mount_for_dev(bnode, existing_mnt, sizeof(existing_mnt),
                                  &rdonly, NULL, 0) && !rdonly) {
      if (mnt_out && cap > 0) snprintf(mnt_out, (size_t)cap, "%s", existing_mnt);
      return 0;
    }
    if (existing_mnt[0] && rdonly) unmount_existing_os_mount(bnode);

    char mnt_dir[PATH_MAX] = {0};
    int created_dir = 0;
    int mounted = 0;
    char dout[512] = {0};

    const char *env_root = getenv("RUFUX_MOUNT_ROOT");
    if (env_root && env_root[0] &&
        make_unique_mount_dir(label, mnt_dir, sizeof(mnt_dir)) == 0) {
      created_dir = 1;
      const char *dav1[] = {"/usr/sbin/diskutil", "mount", "-mountPoint",
                            mnt_dir, bnode, NULL};
      if (rufux_capture(dav1, dout, sizeof(dout)) == 0 &&
          find_active_mount_for_dev(bnode, existing_mnt, sizeof(existing_mnt),
                                    &rdonly, NULL, 0)) {
        snprintf(mnt_dir, sizeof(mnt_dir), "%s", existing_mnt);
        mounted = 1;
      } else {
        rmdir_with_retry(mnt_dir);
        mnt_dir[0] = '\0';
        created_dir = 0;
      }
    }

    if (!mounted) {
      const char *dav2[] = {"/usr/sbin/diskutil", "mount", bnode, NULL};
      if (rufux_capture(dav2, dout, sizeof(dout)) != 0 ||
          !find_active_mount_for_dev(bnode, mnt_dir, sizeof(mnt_dir),
                                    &rdonly, NULL, 0)) {
        set_err(err, errcap, "diskutil mount '%s' failed: %s", bnode, dout);
        return -1;
      }
    }

    MountState ms;
    memset(&ms, 0, sizeof(ms));
    snprintf(ms.kind, sizeof(ms.kind), "diskutil");
    snprintf(ms.dev, sizeof(ms.dev), "%s", dev);
    snprintf(ms.canon_dev, sizeof(ms.canon_dev), "%s", canon_dev);
    snprintf(ms.mnt, sizeof(ms.mnt), "%s", mnt_dir);
    if (!realpath(mnt_dir, ms.canon_mnt)) {
      snprintf(ms.canon_mnt, sizeof(ms.canon_mnt), "%s", mnt_dir);
    }
    ms.created_dir = created_dir;
    (void)write_mount_state(state_dir, &ms);

    if (mnt_out && cap > 0) snprintf(mnt_out, (size_t)cap, "%s", mnt_dir);
    return 0;
  }

  /* Native image file fallback via hdiutil attach */
  char mnt_dir[PATH_MAX];
  if (make_unique_mount_dir(label, mnt_dir, sizeof(mnt_dir)) != 0) {
    set_err(err, errcap, "failed to create mount directory for '%s'", dev);
    return -1;
  }
  char hout[1024] = {0};
  const char *hav[] = {
    "/usr/bin/hdiutil", "attach",
    "-imagekey", "diskimage-class=CRawDiskImage",
    "-readwrite", "-mountpoint", mnt_dir, canon_dev, NULL
  };
  if (rufux_capture(hav, hout, sizeof(hout)) != 0) {
    rmdir_with_retry(mnt_dir);
    set_err(err, errcap, "hdiutil attach '%s' failed: %s", canon_dev, hout);
    return -1;
  }

  char attached_disk[128] = {0};
  const char *dp = strstr(hout, "/dev/disk");
  if (dp) {
    size_t k = 0;
    while (dp[k] && !isspace((unsigned char)dp[k]) && k + 1 < sizeof(attached_disk)) {
      attached_disk[k] = dp[k];
      k++;
    }
    attached_disk[k] = '\0';
  }

  MountState ms;
  memset(&ms, 0, sizeof(ms));
  snprintf(ms.kind, sizeof(ms.kind), "hdiutil");
  snprintf(ms.dev, sizeof(ms.dev), "%s", dev);
  snprintf(ms.canon_dev, sizeof(ms.canon_dev), "%s", canon_dev);
  snprintf(ms.mnt, sizeof(ms.mnt), "%s", mnt_dir);
  if (!realpath(mnt_dir, ms.canon_mnt)) {
    snprintf(ms.canon_mnt, sizeof(ms.canon_mnt), "%s", mnt_dir);
  }
  snprintf(ms.attached_disk, sizeof(ms.attached_disk), "%s", attached_disk);
  ms.created_dir = 1;
  (void)write_mount_state(state_dir, &ms);

  if (mnt_out && cap > 0) snprintf(mnt_out, (size_t)cap, "%s", mnt_dir);
  return 0;
}

int rufux_unmount(const char *dev, int dry_run, char *err, unsigned long errcap) {
  if (err && errcap > 0) err[0] = '\0';
  if (!dev || !dev[0]) {
    set_err(err, errcap, "empty device or mountpoint path");
    return -1;
  }
  if (strcmp(dev, "/") == 0 || rufux_macos_is_boot_disk(dev)) {
    set_err(err, errcap, "refusing to unmount boot/system disk '%s'", dev);
    return -1;
  }
  if (dry_run) return 0;

  char state_dir[PATH_MAX];
  (void)resolve_state_dir(state_dir, sizeof(state_dir));

  MountState ms;
  if (find_matching_state(state_dir, dev, &ms) == 0) {
    char uout[512] = {0};
    if (strcmp(ms.kind, "nfs") == 0) {
      if (is_mountpoint_active(ms.mnt, ms.canon_mnt)) {
        const char *uav1[] = {"/sbin/umount", ms.mnt, NULL};
        if (rufux_capture(uav1, uout, sizeof(uout)) != 0 &&
            is_mountpoint_active(ms.mnt, ms.canon_mnt)) {
          usleep(100000);
          const char *uav2[] = {"/usr/sbin/diskutil", "unmount", ms.mnt, NULL};
          if (rufux_capture(uav2, uout, sizeof(uout)) != 0 &&
              is_mountpoint_active(ms.mnt, ms.canon_mnt)) {
            const char *uav3[] = {"/sbin/umount", "-f", ms.mnt, NULL};
            if (rufux_capture(uav3, uout, sizeof(uout)) != 0 &&
                is_mountpoint_active(ms.mnt, ms.canon_mnt)) {
              set_err(err, errcap, "unmount '%s' failed: %s",
                      ms.mnt, uout[0] ? uout : "mountpoint remains active");
              return -1;
            }
          }
        }
      }
      if (is_mountpoint_active(ms.mnt, ms.canon_mnt)) {
        set_err(err, errcap, "unmount '%s' failed: mountpoint remains active",
                ms.mnt);
        return -1;
      }
      terminate_and_wait_pid(ms.pid, 5000);
      if (pid_is_alive(ms.pid)) {
        set_err(err, errcap,
                "unmounted '%s' from VFS, but unable to terminate rufux-mountd pid %d",
                ms.mnt, (int)ms.pid);
        return -1;
      }
      if (ms.pidfile[0]) (void)unlink(ms.pidfile);
      if (ms.created_dir) rmdir_with_retry(ms.mnt);
      (void)unlink(ms.state_file);
      return 0;
    }

    if (strcmp(ms.kind, "hdiutil") == 0) {
      const char *target = ms.attached_disk[0] ? ms.attached_disk : ms.mnt;
      const char *dav1[] = {"/usr/bin/hdiutil", "detach", target, NULL};
      if (rufux_capture(dav1, uout, sizeof(uout)) != 0) {
        const char *dav2[] = {"/usr/bin/hdiutil", "detach", "-force", target, NULL};
        if (rufux_capture(dav2, uout, sizeof(uout)) != 0) {
          set_err(err, errcap, "hdiutil detach '%s' failed: %s", target, uout);
          return -1;
        }
      }
      if (ms.created_dir) rmdir_with_retry(ms.mnt);
      (void)unlink(ms.state_file);
      return 0;
    }

    /* kind == diskutil */
    const char *utarget = ms.mnt[0] ? ms.mnt : ms.canon_dev;
    const char *dav1[] = {"/usr/sbin/diskutil", "unmount", utarget, NULL};
    if (rufux_capture(dav1, uout, sizeof(uout)) != 0) {
      const char *dav2[] = {"/usr/sbin/diskutil", "unmount", "force", utarget, NULL};
      if (rufux_capture(dav2, uout, sizeof(uout)) != 0) {
        set_err(err, errcap, "diskutil unmount '%s' failed: %s", utarget, uout);
        return -1;
      }
    }
    if (ms.created_dir) rmdir_with_retry(ms.mnt);
    (void)unlink(ms.state_file);
    return 0;
  }

  /* Fallback when unmounting an untracked mountpoint or block device */
  char bnode[PATH_MAX], uout[512] = {0};
  rufux_macos_block_node(dev, bnode, sizeof(bnode));
  char dbsd[64];
  if (parse_bsd_ident(bnode, dbsd, sizeof(dbsd)) && is_whole_disk_bsd_ident(dbsd)) {
    const char *duav[] = {"/usr/sbin/diskutil", "unmountDisk", bnode, NULL};
    if (rufux_capture(duav, uout, sizeof(uout)) == 0) return 0;
  }
  const char *dav[] = {"/usr/sbin/diskutil", "unmount", bnode, NULL};
  if (rufux_capture(dav, uout, sizeof(uout)) == 0) return 0;

  const char *uav[] = {"/sbin/umount", dev, NULL};
  if (rufux_capture(uav, uout, sizeof(uout)) == 0) return 0;

  set_err(err, errcap, "unmount '%s' failed: %s", dev, uout);
  return -1;
}

int rufux_mount_is_whole_disk_active(const char *whole_sysname) {
  char wbsd[64];
  if (!parse_bsd_ident(whole_sysname, wbsd, sizeof(wbsd)) ||
      !is_whole_disk_bsd_ident(wbsd)) {
    return 0;
  }
  char state_dir[PATH_MAX];
  if (resolve_state_dir(state_dir, sizeof(state_dir)) != 0) return 0;

  DIR *d = opendir(state_dir);
  if (!d) return 0;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    size_t len = strlen(de->d_name);
    if (len < 7 || strcmp(de->d_name + len - 6, ".state") != 0) continue;
    char spath[PATH_MAX];
    snprintf(spath, sizeof(spath), "%s/%s", state_dir, de->d_name);
    MountState cur;
    if (read_mount_state_file(spath, &cur) != 0) continue;
    char sbsd[64];
    int match = 0;
    if (parse_bsd_ident(cur.canon_dev, sbsd, sizeof(sbsd)) &&
        bsd_belongs_to_whole_ident(sbsd, wbsd)) match = 1;
    else if (parse_bsd_ident(cur.dev, sbsd, sizeof(sbsd)) &&
             bsd_belongs_to_whole_ident(sbsd, wbsd)) match = 1;
    else if (parse_bsd_ident(cur.attached_disk, sbsd, sizeof(sbsd)) &&
             bsd_belongs_to_whole_ident(sbsd, wbsd)) match = 1;
    if (!match) continue;
    if (!is_mountpoint_active(cur.mnt, cur.canon_mnt)) continue;
    if (strcmp(cur.kind, "nfs") == 0 && !pid_is_alive(cur.pid)) continue;
    closedir(d);
    return 1;
  }
  closedir(d);
  return 0;
}

int rufux_unmount_all_for_disk(const char *disk, char *err, unsigned long errcap) {
  if (err && errcap > 0) err[0] = '\0';
  if (!disk || !disk[0]) return 0;
  char state_dir[PATH_MAX];
  if (resolve_state_dir(state_dir, sizeof(state_dir)) != 0) return 0;

  char bnode[PATH_MAX];
  rufux_macos_block_node(disk, bnode, sizeof(bnode));
  char dbsd[64];
  char whole_target[PATH_MAX];
  if (parse_bsd_ident(bnode, dbsd, sizeof(dbsd))) {
    char *s = strchr(dbsd + 4, 's');
    if (s) *s = '\0';
    snprintf(whole_target, sizeof(whole_target), "/dev/%s", dbsd);
  } else {
    snprintf(whole_target, sizeof(whole_target), "%s", bnode);
  }

  MountState ms;
  int guards = 0;
  while (guards++ < 32 && find_matching_state(state_dir, whole_target, &ms) == 0) {
    const char *t = ms.canon_dev[0] ? ms.canon_dev : (ms.mnt[0] ? ms.mnt : ms.dev);
    if (rufux_unmount(t, 0, err, errcap) != 0) return -1;
  }
  return 0;
}

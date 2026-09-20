#include "exec.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>

// PATH search (mirrors execvp): absolute paths checked directly,
// bare names resolved against each PATH component.
int rufux_have(const char *name) {
  if (!name || !name[0]) return 0;
  if (strchr(name, '/')) return access(name, X_OK) == 0;
  const char *path = getenv("PATH");
  if (!path || !path[0]) path = "/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin";
  char *copy = strdup(path);
  if (!copy) return 0;
  int found = 0;
  for (char *save = NULL, *dir = strtok_r(copy, ":", &save); dir;
       dir = strtok_r(NULL, ":", &save)) {
    char full[1024];
    snprintf(full, sizeof full, "%s/%s", dir, name);
    if (access(full, X_OK) == 0) { found = 1; break; }
  }
  free(copy);
  return found;
}

int rufux_run(const char *const argv[], int dry_run) {
  fprintf(stderr, "+");
  for (int i = 0; argv[i]; i++) fprintf(stderr, " %s", argv[i]);
  fprintf(stderr, "\n");
  if (dry_run) return 0;
  pid_t p = fork();
  if (p < 0) return -1;
  if (p == 0) {
    execvp(argv[0], (char *const *)argv);
    _exit(127);
  }
  int st = 0;
  while (waitpid(p, &st, 0) < 0) {}
  return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

int rufux_capture(const char *const av[], char *out, unsigned long cap) {
  if (!out || cap == 0) return -1;
  out[0] = 0;
  int fd[2];
  if (pipe(fd) != 0) return -1;
  pid_t pid = fork();
  if (pid < 0) { close(fd[0]); close(fd[1]); return -1; }
  if (pid == 0) {
    dup2(fd[1], STDOUT_FILENO);
    dup2(fd[1], STDERR_FILENO);
    close(fd[0]);
    close(fd[1]);
    execvp(av[0], (char *const *)av);
    _exit(127);
  }
  close(fd[1]);
  size_t n = 0;
  ssize_t r;
  char buf[512];
  // Drain to EOF even when the buffer is full: stopping early would
  // leave the child blocked on a full pipe (SIGPIPE death).
  while ((r = read(fd[0], buf, sizeof buf)) > 0) {
    size_t take = (size_t)r;
    if (n < cap - 1) {
      if (n + take >= cap) take = cap - 1 - n;
      memcpy(out + n, buf, take);
      n += take;
    }
  }
  close(fd[0]);
  out[n] = 0;
  int st = 0;
  while (waitpid(pid, &st, 0) < 0) {}
  return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

// ---------------------------------------------------------------------------
// Temp directory selection.
//
// When Rufux runs through pkexec the worker is root, so /tmp (often a small
// RAM disk) is the default.  We prefer locations that the invoking user chose
// (next to the ISO, or in the home directory) because they are on real disk
// and usually have much more room.  The candidate list is:
//
//   1. <iso_dir>/rufux_tmp   — the ISO's own folder (user picked it)
//   2. $HOME/.cache/rufux    — standard cache location
//   3. $HOME                 — any home dir (picks up SUDO_UID's home)
//   4. /var/tmp              — always on real disk
//   5. /tmp                  — last resort (RAM disk on some distros)
//
// Each candidate is tested with statvfs; only directories with at least
// min_bytes free are accepted.  Returns a malloc'd path on success,
// NULL on failure.
// ---------------------------------------------------------------------------

static int dir_has_space(const char *dir, unsigned long long min_bytes) {
  struct statvfs v;
  if (statvfs(dir, &v) != 0) return 0;
  unsigned long long free_bytes = (unsigned long long)v.f_bavail * v.f_frsize;
  return free_bytes >= min_bytes;
}

static int try_mkdir(const char *dir) {
  struct stat st;
  if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) return 1;
  if (mkdir(dir, 0755) == 0) return 1;
  if (errno == EEXIST) return 1;
  return 0;
}

char *rufux_tmpdir_pick(const char *near_iso, unsigned long long min_bytes) {
  const char *home = getenv("HOME");
  // pkexec / sudo stash the real user id in these env vars
  if (!home || !home[0]) {
    const char *uid_str = getenv("PKEXEC_UID");
    if (!uid_str || !uid_str[0]) uid_str = getenv("SUDO_UID");
    if (uid_str && uid_str[0]) {
      uid_t uid = (uid_t)strtoul(uid_str, NULL, 10);
      struct passwd *pw = getpwuid(uid);
      if (pw && pw->pw_dir && pw->pw_dir[0]) home = pw->pw_dir;
    }
  }

  // Build candidate list (dynamic: near_iso is optional)
  const char *cands[8];
  int nc = 0;
  char iso_sibling[1152] = {0};

  // 1. Sibling directory next to the ISO
  if (near_iso && near_iso[0]) {
    const char *slash = strrchr(near_iso, '/');
    if (slash && slash > near_iso) {
      size_t len = (size_t)(slash - near_iso);
      if (len < sizeof iso_sibling - 12) {
        memcpy(iso_sibling, near_iso, len);
        iso_sibling[len] = 0;
        strcat(iso_sibling, "/rufux_tmp");
        cands[nc++] = iso_sibling;
      }
    }
    // 2. The ISO's own directory (writable by root)
    char iso_dir[1152] = {0};
    if (slash && slash > near_iso) {
      size_t len = (size_t)(slash - near_iso);
      if (len < sizeof iso_dir) {
        memcpy(iso_dir, near_iso, len);
        iso_dir[len] = 0;
        cands[nc++] = iso_dir;
      }
    }
  }

  // 3. $HOME/.cache/rufux
  static char home_cache[1152];
  if (home && home[0]) {
    snprintf(home_cache, sizeof home_cache, "%s/.cache/rufux", home);
    cands[nc++] = home_cache;
  }

  // 4. $HOME itself
  if (home && home[0]) cands[nc++] = home;

  // 5. /var/tmp
  cands[nc++] = "/var/tmp";

  // 6. /tmp (last resort)
  cands[nc++] = "/tmp";

  for (int i = 0; i < nc; i++) {
    if (!try_mkdir(cands[i])) continue;
    if (!dir_has_space(cands[i], min_bytes)) continue;
    char *out = strdup(cands[i]);
    if (out) return out;
  }
  return NULL;
}

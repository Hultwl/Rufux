#include "exec.h"
#include "platform_macos.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int is_exec_file(const char *p) {
  struct stat st;
  return p && p[0] && stat(p, &st) == 0 && S_ISREG(st.st_mode) && access(p, X_OK) == 0;
}

static void tool_candidates(const char *name, const char *out[4]) {
  if (strcmp(name, "7z") == 0) {
    out[0] = "7zz";
    out[1] = "7z";
    out[2] = NULL;
  } else if (strcmp(name, "mkfs.ntfs") == 0) {
    out[0] = "mkntfs";
    out[1] = "mkfs.ntfs";
    out[2] = NULL;
  } else if (strcmp(name, "mkfs.ext4") == 0 ||
             strcmp(name, "mkfs.ext3") == 0 ||
             strcmp(name, "mkfs.ext2") == 0) {
    out[0] = name;
    out[1] = "mke2fs";
    out[2] = NULL;
  } else if (strcmp(name, "grub-mkimage") == 0) {
    out[0] = "i686-elf-grub-mkimage";
    out[1] = "grub-mkimage";
    out[2] = NULL;
  } else {
    out[0] = name;
    out[1] = NULL;
  }
}

const char *rufux_resolve_tool(const char *name) {
  static _Thread_local char slots[4][PATH_MAX];
  static _Thread_local unsigned slot_idx = 0;
  static int env_inited = 0;

  if (!name || !name[0]) return NULL;
  if (!env_inited) {
    env_inited = 1;
    rufux_macos_init_bundle_env();
  }

  char *dst = slots[slot_idx++ & 3u];
  if (strchr(name, '/')) {
    if (!is_exec_file(name)) return NULL;
    snprintf(dst, PATH_MAX, "%s", name);
    return dst;
  }

  const char *cands[4];
  tool_candidates(name, cands);

  const char *path = getenv("PATH");
  if (!path || !path[0]) path = "/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin";
  char *copy = strdup(path);
  if (!copy) return NULL;

  int found = 0;
  for (char *save = NULL, *dir = strtok_r(copy, ":", &save); dir && !found;
       dir = strtok_r(NULL, ":", &save)) {
    const char *d = dir[0] ? dir : ".";
    for (int i = 0; cands[i]; i++) {
      snprintf(dst, PATH_MAX, "%s/%s", d, cands[i]);
      if (is_exec_file(dst)) {
        found = 1;
        break;
      }
    }
  }
  free(copy);
  return found ? dst : NULL;
}

int rufux_have(const char *name) {
  return rufux_resolve_tool(name) != NULL ? 1 : 0;
}

static const char *base_name(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

// Builds argv for execv(full, av2). When mkfs.ext{2,3,4} resolves to mke2fs,
// preserves argv[0] so mke2fs sees its mkfs.extN personality AND injects
// "-t <extN>" if neither -t nor -V is already present.
static const char **build_exec_argv(const char *full, const char *const argv[]) {
  int n = 0;
  while (argv[n]) n++;

  const char *b0 = base_name(argv[0]);
  const char *bf = base_name(full);
  int is_mkfs_ext_alias =
      (strcmp(b0, "mkfs.ext2") == 0 ||
       strcmp(b0, "mkfs.ext3") == 0 ||
       strcmp(b0, "mkfs.ext4") == 0) &&
      strcmp(bf, "mke2fs") == 0;

  int inject_type = 0;
  if (is_mkfs_ext_alias) {
    inject_type = 1;
    for (int i = 1; i < n; i++) {
      if (strcmp(argv[i], "--") == 0) break;
      if (strcmp(argv[i], "-t") == 0 ||
          strncmp(argv[i], "-text", 5) == 0 ||
          strcmp(argv[i], "-V") == 0) {
        inject_type = 0;
        break;
      }
    }
  }

  size_t total = (size_t)n + (inject_type ? 2u : 0u) + 1u;
  const char **av2 = malloc(total * sizeof *av2);
  if (!av2) return NULL;

  int o = 0;
  av2[o++] = is_mkfs_ext_alias ? argv[0] : full;
  if (inject_type) {
    av2[o++] = "-t";
    av2[o++] = b0 + 5; // "ext2" | "ext3" | "ext4"
  }
  for (int i = 1; i < n; i++) av2[o++] = argv[i];
  av2[o] = NULL;
  return av2;
}

int rufux_execvp(const char *file, const char *const argv[]) {
  if (!file || !file[0] || !argv || !argv[0]) {
    errno = ENOENT;
    return -1;
  }
  const char *full = rufux_resolve_tool(file);
  if (!full) {
    errno = ENOENT;
    return -1;
  }
  const char **av2 = build_exec_argv(full, argv);
  if (!av2) {
    errno = ENOMEM;
    return -1;
  }
  execv(full, (char *const *)av2);
  int err = errno;
  free(av2);
  errno = err;
  return -1;
}

const char *rufux_exe_dir(void) {
  return rufux_macos_exe_dir();
}

int rufux_pwrite_all(int fd, const void *buf, size_t n, off_t off) {
  const char *p = buf;
  while (n > 0) {
    ssize_t w = pwrite(fd, p, n, off);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return -1;
    p += w;
    off += w;
    n -= (size_t)w;
  }
  return 0;
}

ssize_t rufux_read_full(int fd, void *buf, size_t n) {
  char *p = buf;
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(fd, p + got, n - got);
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) return -1;
    if (r == 0) break;
    got += (size_t)r;
  }
  return (ssize_t)got;
}

int rufux_run(const char *const argv[], int dry_run) {
  if (!argv || !argv[0]) return -1;
  fprintf(stderr, "+");
  for (int i = 0; argv[i]; i++) fprintf(stderr, " %s", argv[i]);
  fprintf(stderr, "\n");
  if (dry_run) return 0;

  const char *full = rufux_resolve_tool(argv[0]);
  if (!full) return -1;

  const char **av2 = build_exec_argv(full, argv);
  if (!av2) return -1;

  pid_t p = fork();
  if (p < 0) {
    free(av2);
    return -1;
  }
  if (p == 0) {
    execv(full, (char *const *)av2);
    _exit(127);
  }
  free(av2);
  int st = 0;
  while (waitpid(p, &st, 0) < 0) {
    if (errno != EINTR) return -1;
  }
  return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

int rufux_capture(const char *const av[], char *out, unsigned long cap) {
  if (!out || cap == 0 || !av || !av[0]) return -1;
  out[0] = 0;

  const char *full = rufux_resolve_tool(av[0]);
  if (!full) return -1;

  const char **av2 = build_exec_argv(full, av);
  if (!av2) return -1;

  int fd[2];
  if (pipe(fd) != 0) {
    free(av2);
    return -1;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(fd[0]);
    close(fd[1]);
    free(av2);
    return -1;
  }
  if (pid == 0) {
    dup2(fd[1], STDOUT_FILENO);
    dup2(fd[1], STDERR_FILENO);
    close(fd[0]);
    close(fd[1]);
    execv(full, (char *const *)av2);
    _exit(127);
  }
  free(av2);
  close(fd[1]);

  size_t n = 0;
  char buf[512];
  for (;;) {
    ssize_t r = read(fd[0], buf, sizeof buf);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
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
  while (waitpid(pid, &st, 0) < 0) {
    if (errno != EINTR) return -1;
  }
  return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

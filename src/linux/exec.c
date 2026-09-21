#include "exec.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
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

// Directory containing the running executable, via /proc/self/exe.
// Lets relocatable bundles (AppImage) find data next to the binary:
// <exedir>/../share/... mirrors a /usr install prefix.
const char *rufux_exe_dir(void) {
  static char dir[1024] = {0};
  static int done = 0;
  if (!done) {
    done = 1;
    ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
    if (n > 0) {
      dir[n] = 0;
      char *s = strrchr(dir, '/');
      if (s) *s = 0;
      else dir[0] = 0;
    }
  }
  return dir;
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

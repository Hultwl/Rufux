// LD_PRELOAD shim: redirect WebKitGTK's helper executables (and its
// injected bundle) from the distribution-baked absolute directory to the
// copy that ships inside the AppImage.
//
// Why: Ubuntu builds WebKitGTK without DEVELOPER_MODE, so WEBKIT_EXEC_PATH
// is ignored and the helpers (WebKitWebProcess, WebKitNetworkProcess) are
// only looked up under the baked PKGLIBEXECDIR, e.g.
// /usr/lib/x86_64-linux-gnu/webkit2gtk-4.1 - an absolute path that cannot
// exist on the host. Only basenames are matched here, so no host paths
// are hardcoded and the shim is harmless everywhere else.
//
// Loaded via LD_PRELOAD by the backend stub (gui_stub.c) before it execs
// the GUI; inherited by the helper processes themselves.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const kProcs[] = {
  "WebKitWebProcess", "WebKitNetworkProcess", "WebKitGPUProcess", NULL,
};
static const char *const kBundledDir = "webkit2gtk-4.1";

static const char *base(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

// <out> = directory holding the bundled helpers:
// $APPDIR/usr/lib/webkit2gtk-4.1 (AppImage runtime exports APPDIR),
// else <this-exe-dir>/../lib/webkit2gtk-4.1 (plain installs).
static void helpers_dir(char *out, size_t cap) {
  const char *appdir = getenv("APPDIR");
  if (appdir && *appdir) {
    snprintf(out, cap, "%s/usr/lib/%s", appdir, kBundledDir);
    return;
  }
  char self[4096] = {0};
  ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
  if (n > 0) {
    self[n] = 0;
    char *slash = strrchr(self, '/');
    if (slash) {
      *slash = 0;
      snprintf(out, cap, "%s/../lib/%s", self, kBundledDir);
      return;
    }
  }
  snprintf(out, cap, "/usr/lib/%s", kBundledDir);
}

static int is_helper(const char *path) {
  const char *b = base(path);
  for (int i = 0; kProcs[i]; i++)
    if (!strcmp(b, kProcs[i]))
      return 1;
  return 0;
}

typedef int (*execve_fn)(const char *path, char *const argv[], char *const envp[]);
typedef int (*execvpe_fn)(const char *file, char *const argv[], char *const envp[]);
typedef void *(*dlopen_fn)(const char *file, int mode);
typedef int (*spawn_fn)(pid_t *pid, const char *path,
                        const posix_spawn_file_actions_t *fa,
                        const posix_spawnattr_t *attr,
                        char *const argv[], char *const envp[]);

// glib's g_spawn (what WebKit uses) goes through posix_spawn, not raw
// execve - cover both, same basename match.
static int spawn_redirect(const char *path, char *redir, size_t cap) {
  if (!path || !is_helper(path))
    return 0;
  char dir[4096];
  helpers_dir(dir, sizeof dir);
  snprintf(redir, cap, "%s/%s", dir, base(path));
  return !access(redir, X_OK);
}

int execve(const char *path, char *const argv[], char *const envp[]) {
  static execve_fn real = NULL;
  if (!real)
    real = (execve_fn)dlsym(RTLD_NEXT, "execve");
  if (path && is_helper(path)) {
    char dir[4096], redir[4352];
    helpers_dir(dir, sizeof dir);
    snprintf(redir, sizeof redir, "%s/%s", dir, base(path));
    if (!access(redir, X_OK))
      return real(redir, argv, envp);
  }
  return real(path, argv, envp);
}

int execvpe(const char *file, char *const argv[], char *const envp[]) {
  static execvpe_fn real = NULL;
  if (!real)
    real = (execvpe_fn)dlsym(RTLD_NEXT, "execvpe");
  if (file && is_helper(file)) {
    char dir[4096], redir[4352];
    helpers_dir(dir, sizeof dir);
    snprintf(redir, sizeof redir, "%s/%s", dir, base(file));
    if (!access(redir, X_OK))
      return real(redir, argv, envp);
  }
  return real(file, argv, envp);
}

void *dlopen(const char *file, int mode) {
  static dlopen_fn real = NULL;
  if (!real)
    real = (dlopen_fn)dlsym(RTLD_NEXT, "dlopen");
  if (file && !strcmp(base(file), "libwebkit2gtkinjectedbundle.so")) {
    char dir[4096], redir[4352];
    helpers_dir(dir, sizeof dir);
    snprintf(redir, sizeof redir, "%s/injected-bundle/libwebkit2gtkinjectedbundle.so", dir);
    if (!access(redir, R_OK)) {
      void *h = real(redir, mode);
      if (h)
        return h;
    }
  }
  return real(file, mode);
}

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *attr,
                char *const argv[], char *const envp[]) {
  static spawn_fn real = NULL;
  if (!real)
    real = (spawn_fn)dlsym(RTLD_NEXT, "posix_spawn");
  char redir[4352];
  if (spawn_redirect(path, redir, sizeof redir))
    return real(pid, redir, fa, attr, argv, envp);
  return real(pid, path, fa, attr, argv, envp);
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *attr,
                 char *const argv[], char *const envp[]) {
  static spawn_fn real = NULL;
  if (!real)
    real = (spawn_fn)dlsym(RTLD_NEXT, "posix_spawnp");
  char redir[4352];
  if (spawn_redirect(file, redir, sizeof redir))
    return real(pid, redir, fa, attr, argv, envp);
  return real(pid, file, fa, attr, argv, envp);
}

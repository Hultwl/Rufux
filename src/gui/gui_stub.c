// CLI-only builds: the graphical interface lives in gui-tauri/ (a separate
// Tauri crate driving this backend over its CLI).
//
// AppImage routing depends on this stub: AppRun points at the backend, so
// `AppImage [backend args...]` (including `pkexec $APPIMAGE create ...`)
// lands here. With no arguments and a display available, hand off to the
// bundled GUI next to this binary instead of printing usage.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int rufux_gui_run(int argc, char **argv) {
  (void)argc;
  if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) {
    fprintf(stderr,
            "This build of Rufux has no graphical interface linked in: run "
            "gui-tauri/ for the window, or use the command line (run rufux "
            "with no arguments for the options).\n");
    return 1;
  }
  char self[4096] = {0};
  ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
  if (n <= 0)
    return 1;
  self[n] = 0;
  char *slash = strrchr(self, '/');
  if (!slash)
    return 1;
  // WebKitGTK is bundled now (AppImageHub requires self-containment), so
  // the bundle's libraries must WIN - leave LD_LIBRARY_PATH alone. The
  // AppImage runtime sets it to the bundle lib dir; the GUI binary also
  // carries an $ORIGIN RUNPATH for the same reason.
  //
  // Two more environment setups for the bundled WebKitGTK, both harmless
  // on plain installs (paths simply won't exist there):
  // - WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS: the renderer sandbox
  //   needs unprivileged user namespaces, which sandboxes-within-sandbox
  //   (firejail, flatpak-style CI) deny. The window only ever renders our
  //   own local UI, never remote web content (downloads run in the curl
  //   backend), so there is no remote attack surface to sandbox.
  // - LD_PRELOAD the exec shim: Ubuntu builds WebKitGTK without
  //   DEVELOPER_MODE, so helpers are only looked up under the baked
  //   absolute PKGLIBEXECDIR. The shim redirects those execs/dlopens to
  //   the bundled copy (see src/shim/webkit_shim.c).
  setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", 1);
  {
    char shim[4352];
    snprintf(shim, sizeof shim, "%.*s/../lib/rufux-webkit-shim.so",
             (int)(slash - self), self);
    if (!access(shim, R_OK))
      setenv("LD_PRELOAD", shim, 1);
  }
  // The GUI binary lives next to the backend everywhere (AppImage,
  // /usr/bin, development trees alike).
  static const char *names[] = {"rufux-gui", NULL};
  for (int i = 0; names[i]; i++) {
    char path[4192];
    snprintf(path, sizeof path, "%.*s/%s", (int)(slash - self), self, names[i]);
    if (!access(path, X_OK)) {
      argv[0] = path;
      execv(path, argv);
      break;  // exec failed; try the next name, then give up below
    }
  }
  fprintf(stderr, "rufux: graphical interface not found next to %s\n", self);
  return 1;
}

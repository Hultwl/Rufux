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
  // The window renders in host WebKitGTK (too big to bundle, must match
  // the host graphics stack). Scrub the bundle loader path here - a
  // wrapper script cannot do it reliably ($0 is the AppImage, not the
  // wrapper, after the handoff) - so every library resolves to the
  // system. Harmless on plain installs where it is already unset.
  unsetenv("LD_LIBRARY_PATH");
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

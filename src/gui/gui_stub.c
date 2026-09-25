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
  (void)argv;
  if (getenv("DISPLAY") || getenv("WAYLAND_DISPLAY")) {
    char self[4096] = {0};
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n > 0) {
      self[n] = 0;
      char *slash = strrchr(self, '/');
      if (slash) {
        // Prefer the wrapper (it drops the bundle loader path so the host
        // WebKit never mixes with the bundled glib), fall back to the raw
        // binary (developer trees, AUR without wrapper).
        static const char *names[] = {"rufux-gui", "rufux-gui.bin", NULL};
        for (int i = 0; names[i]; i++) {
          char path[4192];
          snprintf(path, sizeof path, "%.*s/%s", (int)(slash - self), self, names[i]);
          if (!access(path, X_OK)) {
            execv(path, argv);
            break;  // exec failed; fall through to the message
          }
        }
      }
    }
  }
  fprintf(stderr,
          "This build of Rufux has no graphical interface linked in: run "
          "gui-tauri/ for the window, or use the command line (run rufux "
          "with no arguments for the options).\n");
  return 1;
}

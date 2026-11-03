// CLI-only builds: the graphical interface lives in macos/gui-tauri/ (a separate
// Tauri crate driving this backend over its CLI).
#include "exec.h"
#include "gui/gui.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int rufux_gui_run(int argc, char **argv) {
  (void)argc;
  const char *ed = rufux_exe_dir();
  if (ed && ed[0] && !getenv("RUFUX_NO_GUI_HANDOFF")) {
    const char *candidates[] = {
      "rufux-gui",
      "../../MacOS/rufux-gui",
      NULL
    };
    for (int i = 0; candidates[i]; i++) {
      char path[PATH_MAX];
      snprintf(path, sizeof path, "%s/%s", ed, candidates[i]);
      if (access(path, X_OK) == 0) {
        argv[0] = path;
        execv(path, argv);
        break;
      }
    }
  }
  fprintf(stderr,
          "This build of Rufux has no graphical interface linked in: launch "
          "Rufux.app (gui-tauri) for the window, or use the command line.\n");
  return 1;
}

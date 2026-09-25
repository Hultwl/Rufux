// CLI-only builds: the graphical interface lives in gui-tauri/ (a separate
// Tauri crate driving this backend over its CLI). main.c still references
// rufux_gui_run, so provide one that explains itself.
#include <stdio.h>

int rufux_gui_run(int argc, char **argv) {
  (void)argc;
  (void)argv;
  fprintf(stderr,
          "This build of Rufux has no graphical interface: build gui-tauri/ "
          "for the window, or use the command line (run rufux with no "
          "arguments for the options).\n");
  return 1;
}

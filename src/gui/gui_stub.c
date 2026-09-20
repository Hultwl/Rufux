// CLI-only builds: Qt6 was not found, so there is no graphical interface.
// main.c still references rufux_gui_run, so provide one that explains itself.
#include <stdio.h>

int rufux_gui_run(int argc, char **argv) {
  (void)argc;
  (void)argv;
  fprintf(stderr,
          "This build of Rufux has no graphical interface: it was compiled "
          "without Qt6.\nInstall the Qt6 Widgets development package and "
          "rebuild, or use the command line (run rufux with no arguments for "
          "the options).\n");
  return 1;
}

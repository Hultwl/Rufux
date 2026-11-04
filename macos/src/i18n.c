#include "i18n.h"
#ifdef ENABLE_NLS
#include "exec.h"
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <sys/stat.h>
#ifndef RUFUX_LOCALEDIR
#define RUFUX_LOCALEDIR "/usr/share/locale"
#endif

void rufux_i18n_init(void) {
  setlocale(LC_ALL, "");
  const char *dir = RUFUX_LOCALEDIR;
  static char rel[PATH_MAX];
  const char *ed = rufux_exe_dir();
  struct stat st;
  if (ed[0]) {
    snprintf(rel, sizeof rel, "%s/../Resources/share/locale", ed);
    if (stat(rel, &st) == 0 && S_ISDIR(st.st_mode)) {
      dir = rel;
    } else {
      snprintf(rel, sizeof rel, "%s/../share/locale", ed);
      if (stat(rel, &st) == 0 && S_ISDIR(st.st_mode)) dir = rel;
    }
  }
  bindtextdomain("rufux", dir);
  textdomain("rufux");
}
#else
void rufux_i18n_init(void) {}
#endif

#ifndef RUFUX_I18N_H
#define RUFUX_I18N_H
// gettext wrapper. Identity macro when NLS is off so no extra dep.
#ifdef ENABLE_NLS
#include <libintl.h>
#include <locale.h>
#include <sys/stat.h>
#include <stdio.h>
#include "exec.h"
#ifndef RUFUX_LOCALEDIR
#define RUFUX_LOCALEDIR "/usr/share/locale"
#endif
#define _(s) gettext(s)
static inline void rufux_i18n_init(void) {
  setlocale(LC_ALL, "");
  // Relocatable bundles (AppImage) carry their own catalogs at
  // <exedir>/../share/locale; prefer them when present.
  const char *dir = RUFUX_LOCALEDIR;
  static char rel[1152];
  const char *ed = rufux_exe_dir();
  struct stat st;
  if (ed[0]) {
    snprintf(rel, sizeof rel, "%s/../share/locale", ed);
    if (stat(rel, &st) == 0 && S_ISDIR(st.st_mode)) dir = rel;
  }
  bindtextdomain("rufux", dir);
  textdomain("rufux");
}
#else
#define _(s) (s)
static inline void rufux_i18n_init(void) {}
#endif
#endif

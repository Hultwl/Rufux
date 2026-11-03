#ifndef RUFUX_I18N_H
#define RUFUX_I18N_H
// gettext wrapper. Identity macro when NLS is off so no extra dep.
#ifdef ENABLE_NLS
#include <libintl.h>
#define _(s) gettext(s)
#else
#define _(s) (s)
#endif

void rufux_i18n_init(void);
#endif

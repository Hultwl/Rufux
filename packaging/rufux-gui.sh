#!/bin/sh
# Launcher for the Tauri GUI.
# WebKitGTK always comes from the host (too big to bundle, must match the
# host graphics stack). A host WebKit linked against a newer glib breaks
# when an AppImage bundle's older glib shadows it
# ("undefined symbol: g_sort_array"), so scrub the bundle loader path and
# let every library resolve to the system before starting the real binary.
unset LD_LIBRARY_PATH
here="$(dirname "$(readlink -f "$0")")"
exec "$here/rufux-gui.bin" "$@"

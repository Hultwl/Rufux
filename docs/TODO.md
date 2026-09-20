# TODO

## Known problems

- **Legacy BIOS boot for Windows media.** Not available on NTFS sticks
  (see PORTING.md). Options: FAT32 with the Windows FAT32 boot record
  when every file is under 4 GiB, or FAT32 plus split WIM (wimlib) for
  larger images. The FAT32 record can be tested in QEMU; this sandbox
  had no `vfat` module, so it needs a machine that does.
- **Confirm the Windows Setup fix.** 1.2.7 changed the Windows layout to
  match Rufus. Someone with a real Windows ISO needs to burn a stick and
  confirm Setup finds `install.wim` (see the 1.2.7 changelog).
- `install.wim` larger than 4 GiB has not been tested; the NTFS path should
  handle it.

## Qt6 GUI (in 1.3.0, needs a real-hardware test)

The Qt interface exists (`src/gui/gui_qt.cpp`) and renders correctly
offscreen. Not yet checked: a real burn started from the window, the
CANCEL button (it terminates pkexec; whether the root worker stops
with it is unverified), the AppImage workflow, and translations (the
Qt strings are English only). `screenshots/rufux-main.png` still shows
the old GTK window and should be replaced.

## Planned: core rewrite (not started)

Restructure the core as a library plus a CLI plus the GUI.

- Core as a C++17 library with no GUI dependency; `plan` then `run` steps
  so `--dry-run` and real runs share one code path.
- Read ISOs by loop-mounting UDF/ISO9660 when possible (exact progress and
  size checks), falling back to 7z.
- CMake build, Qt6 Widgets GUI, CLI kept compatible.
- Keep `tests/test_windows_layout.sh` and the QEMU/OVMF boot check as the
  regression tests for the Windows layout. The helpers are in `tests/`
  (`make_fake_winiso.sh`, `shims/udisksctl`). Setting `FAKE_EFI` builds a
  fake ISO whose EFI loader prints a marker, which proves the UEFI:NTFS
  chain reached the NTFS partition.
- Do the rewrite on its own branch and merge after a real-hardware test;
  the AUR package `rufux-git` builds from `main`.

## Windows To Go (parked)

Recipe in `docs/wintogo-research.md`. Blocked on hardware: a fast 32 GB+
USB stick and a Windows ISO for boot testing.

## Ideas

- UDisks2 D-Bus backend.
- More translations beyond English, French and Spanish.

## Windows customization

Not done yet from Rufus's list: local account creation, copying the host's
locale, disabling BitLocker auto-encryption, "skip disk selection" silent
install, and the 2023 boot loaders option.

## CI

`docs/ci-qt.patch` moves the CI and AppImage workflows to Qt6. It could
not be pushed with the token used so far (no `workflow` scope). Apply it
with `git apply docs/ci-qt.patch`, push, and check that the AppImage
still builds and starts. Until then the AppImage is built from the GTK
fallback.

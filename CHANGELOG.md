# Changelog

## 1.4.1 (device list fix, modern look)

- Fixed: a 16 GB stick could show up as `0.00B` and the write then failed
  with "device too small". Linux lists every card-reader slot as a disk,
  and an empty slot reports a capacity of 0. Those entries are now hidden
  (Rufus does the same), and if a zero-capacity target is chosen anyway the
  error says what it is and what to do, instead of "too small".
- The size read used by the worker also falls back to sysfs, and its error
  message now includes the size it saw.
- New look for the Qt window: soft light and dark themes that follow the
  system, rounded inputs and buttons, a slim progress bar with a separate
  status line (green when ready, red when it fails), a quieter section
  layout, and a red CANCEL while a write runs. Device names read like
  `SanDisk Ultra (sdb) [14.9 GB]`.
- `RUFUX_GUI_DEMO=1` adds a fake device to the list, for screenshots.
- README screenshot updated.

## 1.4.0 (Windows customization rewritten like Rufus)

- **Secure Boot / TPM / RAM bypass now works under the hood.** The
  `LabConfig` keys are written into the SYSTEM registry hive inside
  `sources/boot.wim` (image 2), read back, and checked, exactly as Rufus
  does. Setup's screens stay untouched. This needs `wimlib-imagex` and
  `hivexsh`; without them Rufux falls back to an answer file and tells
  you that the fallback changes Setup's first screens.
- **The answer file no longer forces Setup into partly unattended mode.**
  Before, every customization wrote a `windowsPE` pass (with `UserData`),
  even for "no online account" alone. Rufus documents that such a pass
  alters the installer's flow. Now the `windowsPE` pass exists only in the
  fallback. Everything else goes to `sources/$OEM$/$$/Panther/unattend.xml`,
  where Rufus puts it, and the architecture (amd64, arm64, x86) is taken
  from the media instead of being hardcoded.
- **Driver injection: `--drivers DIR`** (and a folder picker in the GUI's
  Windows options). The folder is copied to `$WinPEDriver$`, which Setup
  loads automatically. Use it when Setup says a media driver is missing or
  shows no drives, for example with Intel RST/VMD controllers.
- New test: tests/test_wue.sh edits a real two-image WIM with a real
  registry hive and checks the bypass, image 1 untouched, the answer-file
  placement, the drivers, and the fallback.

## 1.3.0 (Qt6 interface, extract-mode fix)

- Fixed: `extract` mode (and the "Write in ISO Image mode" option) put
  the ISO on a 512 MiB partition and left the second one unused, so any
  ISO over about 500 MB failed with "No space left on device". It now
  uses one data partition. tests/test_extract_layout.sh covers it with a
  600 MiB image and fails on the old behaviour.
- Fixed: single-partition GPT sticks (extract and "Non bootable") were
  typed as Linux filesystem data even for FAT32/NTFS/exFAT. Windows
  ignores such partitions, so they got no drive letter. They are now
  typed Microsoft basic data; only ext* keeps the Linux type.

- New Qt6 interface laid out like Rufus: Drive Properties, Format
  Options and Status sections, a green progress bar, START/CLOSE, a log
  window with Save, a checksum window (MD5, SHA-1, SHA-256, SHA-512), and
  the Windows User Experience dialog. It still runs writes in a separate
  root process (`pkexec rufux create ... --real --yes`).
- CMake picks Qt6 first, then GTK4, then builds CLI-only. Force one with
  `-DRUFUX_GUI=qt|gtk|none`. The GTK code is still in the tree.
- Package dependencies, CI and the AppImage workflow moved from gtk4 to
  qt6-base (linuxdeploy-plugin-qt).
- `RUFUX_GUI_SNAPSHOT=out.png rufux --gui` saves a screenshot and exits
  (`RUFUX_GUI_IMAGE=file.iso` loads an image first); handy for tests.

## 1.2.7 (Windows media layout, mount path fix)

- Windows install sticks now use the layout Rufus uses: the NTFS data
  partition first, then a 1 MiB UEFI:NTFS partition at the very end.
  Before, a 512 MiB partition typed "EFI System" came first. Rufus's
  own source notes that Windows Setup fails with two ESPs and depends on
  how Windows mounts several partitions on a removable drive; the
  symptom was a stick that boots and then reports "a media driver your
  computer needs is missing". The small partition is typed basic data and
  flagged no-drive-letter. I could not reproduce the Setup error without
  a Windows ISO and hardware, so please report back whether this fixes it.
- The UEFI:NTFS image is written raw to its partition and read back, as
  Rufus does, instead of being unpacked and copied.
- After extraction the Windows tree is checked (bootmgr, boot.wim,
  install.wim/esd/swm); a partial copy now fails the burn.
- Fixed: the mount path reported by udisksctl was cut at the first dot,
  so a label like "Win11.ISO" made the extraction go to a truncated path.
- mkfs.ntfs now gets the partition start sector explicitly.
- The GPT Windows flow no longer needs syslinux.
- The MBR variant of the Windows flow is UEFI-only, and says so. Booting
  it on legacy BIOS never worked: the NTFS boot sector jumps into sectors
  1-15 of $Boot, which mkfs.ntfs leaves empty.
- New test: tests/test_windows_layout.sh burns a fake Windows ISO onto a
  loop device and checks the partition table, the UEFI:NTFS image and the
  copied tree (skipped without root or the needed tools).

## 1.2.6 (mount race + honest failure reasons)

- Burns twice died seconds after formatting while the same mount
  worked by hand: fresh signatures can still be settling inside
  udisksd, so there is now a rescan after formatting plus one
  rescan-and-retry on each mount before failing.
- The GUI failure dialog no longer shows command-echo lines as the
  reason (only real error text).

## 1.2.5 (answer file aligned with Rufus)

- autounattend.xml rewritten against upstream Rufus wue.c: wcm
  namespaces on every component, wcm:action on list items (Setup
  ignored them without it), HW bypasses as WinPE reg commands,
  empty product key block, BitLocker device-encryption guard.
  Bypass/NRO/privacy theoretically apply for real now.

## 1.2.4 (hotfix: data partition type + loud GUI failures)

- GPT data partition is typed Microsoft basic data now (was Linux
  filesystem type) — matches Rufus; Windows Setup sees a Windows
  data volume.
- GUI failures carry the reason: worker pipes drain to EOF and the
  error dialog shows the worker's last error line instead of "see
  the log" above an empty log.
- Includes the 1.2.3 boot fix (full UEFI:NTFS tree: loader plus
  EFI/Rufus NTFS/exFAT drivers, so no more "couldn't find/load
  NTFS driver") and the Windows User Experience dialog.

## 1.2.3 (UEFI:NTFS drivers + Windows User Experience dialog)

- Windows USBs booted the UEFI:NTFS loader but stopped at
  "couldn't find/load NTFS driver": only bootx64.efi was staged.
  The loader needs its NTFS driver beside it, so the ESP now gets
  the full EFI tree (loaders plus EFI/Rufus ntfs/exfat drivers)
  unpacked from the in-tree uefi-ntfs.img — no download needed,
  works offline.
- Windows installation mode now asks first: a Windows User
  Experience dialog on START (remove RAM/Secure Boot/TPM checks,
  online-account requirement, data collection), and the choice is
  actually passed to the worker (previously the GUI selection was
  dropped).

## 1.2.2 (UDF extraction fix + GUI cleanup)

- Windows (UDF) ISOs extracted only the first file while reporting
  success: bsdtar lists some UDF layouts as a single README and exits
  0, so the disk was left with one file and "Done". UDF images now
  route to 7z, non-UDF images keep bsdtar-first with a 7z fallback,
  and an extraction that lands far short of the image size fails
  loudly instead of finishing green.
- Fixed a deadlock in command output capture: it stopped reading once
  the buffer filled, which killed large-output tools (SIGPIPE) and
  corrupted the result.
- Windows ISO detection is now UDF-aware (probe reports udf: yes).
- GUI: labels moved into a right-aligned column with controls beside
  them (the Rufus shape) instead of stacked on their own rows, the
  paired controls align to that column, persistence has its own row,
  the form scrolls so START/CLOSE cannot be clipped, and the
  boot-selection combo shows the picked image's name.

## 1.2.1 (BIOS bootability)

- MBR partitions get filesystem-correct types (vfat 0c, ntfs/exfat
  07) plus the bootable flag — legacy BIOS media actually boots
  now (proven the old table was type 83, unflagged).
- Syslinux chainloader wired into BIOS flows (was dead code);
  warns loudly when the binary is missing.
- `--wue none` disables cleanly instead of erroring.
- mkfs dead argv arrays removed.

- FreeDOS bootable disks: DOS partition + FAT32 + KERNEL.SYS-first
  copy + FreeDOS boot record (ms-sys blobs, Linux-native writer) +
  DOS MBR. Boot picker entry included, byte-exact tested.
- Windows installation media: ESP + NTFS, ISO extract, UEFI:NTFS
  loader fetched from upstream and cached, autounattend.xml with
  Win11 requirement bypasses (TPM/SB/RAM/CPU/storage), NRO and
  privacy options. GUI auto-detects Windows ISOs.
- Honest refusals: ReFS (no Linux formatter), Windows ISO
  downloader (`download-windows` explains the manual path).
- Scope table updated: what shipped vs what stays out and why.

- Checksums x4: md5/sha1/sha256/sha512 (EVP), CLI `--algo`,
  GUI dialog shows all four.
- Fixed VHD images: footer verified, payload-only writes;
  dynamic/VHDX refused with a qemu-img pointer.
- Bad-blocks write patterns (0xAA/0x55/0xFF/0x00, rotating
  1-4 passes) as the pre-write gate and standalone command.
- Scope table in PORTING.md: what's ported, what's honestly
  out of scope, and why.

- Portal theme fix: unwrap variant-wrapped color-scheme replies
  (COSMIC answers on the legacy namespace); proven live.
- No Flatpak: sandbox and raw disks don't mix, so all Flatpak
  packaging left the tree (manifest, metadata, guard, docs).

- Partition rescan actually runs (was log-only): standalone
  `partition` rescans via partprobe + udevadm settle.
- Honest UEFI validation message: header/subsystem check only,
  no signature verified.
- Per-filesystem label limits everywhere (vfat 11, exfat 15,
  ext 16, ntfs/udf 32): GUI truncates, CLI fails fast.
- Flatpak: block devices refused up front with directions
  (no host pkexec path); scope documented in packaging/README.
- Version drift guard: tests/test_packaging.sh asserts CMake,
  PKGBUILD, Flatpak manifest, and CHANGELOG agree.
- No more shell-outs: udisksctl via fork+execvp capture,
  du via nftw, curl/bootctl via shared rufux_capture helper.
- SHA-256 now OpenSSL EVP (hand-rolled implementation deleted);
  build requires libcrypto.
- El Torito parsed structurally (catalog validation-entry
  platform id) instead of byte-scanning for 0xEF.
- mkfs argv on stack (reentrant); persist checks tools before
  truncating; badblocks labeled a read-only surface scan.

- UI stays alive during burns: worker pipes drain non-blocking
  (previously the window froze through long silent phases).
- Theme follows the desktop via the Settings portal (both
  namespaces), GTK settings.ini, then COSMIC-dark default.

- Worker stderr now streams into the GUI log (auth failures, refusal
  reasons) plus the worker exit code — failures are never silent.
- The worker dismounts the target's own partitions before touching
  it (Rufus behavior; consent was the START warning) instead of
  refusing auto-mounted sticks.

- Real progress bar: every flow reports staged percent end to end
  (bad-blocks 0-10, zero 10-15, write 15-85, verify 85-100; extract
  8-82 via destination-growth polling, rest named stages). CLI shows
  speed + ETA; GUI status shows live percent.
- Extraction progress for CLI `extract` too (was silent).
- Run safety: START/CLOSE lock while the worker runs (no double
  burns, no closing mid-write); writer refuses source == target.
- vfat + >4GiB image refused early with an NTFS/exFAT pointer
  (FAT32 cannot hold such files; UEFI:NTFS driver is future work).

- Consolidated stable release: everything below in one cut.
- AppImage attached to the release (built by CI on Ubuntu 24.04).
- Bare `rufux` with a display opens the GUI (app-grid friendly).
- Root escalation that works from AppImages: resolve the real
  executable (readlink, not /proc/self/exe through env) and
  re-run the $APPIMAGE file itself (FUSE mounts are user-private,
  root gets EACCES inside them).
- GUI runs as the invoking user; START escalates per-operation
  (pkexec worker with streamed progress) instead of running the
  whole app as root. Kills the root-on-Wayland display failures,
  theme loss, portal loss, and dconf spam at the root.

## v1.0.3

- Fix GPT ESP type: real GUID C12A7328-F81F-11D2-BA4B-00A0C93EC93B
  (was a literal placeholder) + explicit portable sfdisk lines.
- Fix stack buffer overflow in update-check error path (bound is
  now the 128-byte stack buffer, not the caller's errcap).
- Fix CLI-only link failure: gui stub always compiles.
- Portable tool lookup: bare names resolved via PATH instead of
  hardcoded /usr/bin (Debian/Ubuntu keep mkfs.* in /usr/sbin).
- CI: install ntfs-3g for the NTFS format test.

## v1.0.2

- GUI polish: portal-native file pickers (GtkFileDialog opens the
  system file manager), theme inheritance (flag > rufux config >
  GTK settings > COSMIC dark default), log timestamps, app icon
  installed; dconf silenced for root sessions.

## v1.0.1

- Rename: Lufus → Rufux. The name Lufus belongs to the established
  Hogjects/Lufus project (Python, MIT); this port rebrands to avoid
  confusion. Binary, app ID, locales, docs, and packaging all renamed.
  No functional changes.

## v1.0.0 (Phase 3 — Stable)

- udisks2 auto-mount: `mount` / `umount`, `create --mode extract` end-to-end on disks
- Privilege guard: clear sudo/pkexec error + polkit policy installed
- Secure Boot: `secureboot-status`, `validate-efi` (PE subsystem check)
- `update-check` against GitHub releases
- i18n infrastructure (`po/`, fr + es samples, `ENABLE_NLS`)
- GUI: `--theme system|dark|light`, Secure Boot status line
- Packaging: `packaging/PKGBUILD`, Flatpak manifest, man page, desktop file
- CI: `.github/workflows/rufux.yml` (build + ctest + artifact)
- `tests/HW_MATRIX.md` + `tests/hw_smoke.sh` for manual hardware validation

## v0.3.0 (Phase 2 — Bootable Parity)

- sfdisk partition, mkfs dispatch, ISO extract, syslinux MBR,
  persistence file, bad-blocks scan, `create` planner, GUI mode/scheme/fs

## v0.2.0 (Phase 1 — Safe Core)

- device scan, ISO probe, SHA-256, safe writer, GTK skeleton, tests

## v0.1.0

- Initial scaffold forked from pbatard/rufus.

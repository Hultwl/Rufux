# Changelog

## 1.5.1 (hotfix: temp dir on real disk when running as root)

- **Fixed: worker ran out of space on `/tmp` (often a small RAM disk).**
  When the GUI escalates via `pkexec`, the worker runs as root and
  previously defaulted to `/tmp` for all staging and scratch files.
  On many distros `/tmp` is a tmpfs backed by RAM (often 2-4 GiB),
  which is not enough for large ISOs or Windows WIM images. Rufux now
  picks a writable temp directory on real disk: first a `rufux_tmp/`
  folder next to the ISO, then `$HOME/.cache/rufux`, then `/var/tmp`,
  and only falls back to `/tmp` as a last resort. Each candidate is
  checked with `statvfs` to ensure sufficient free space.
- The UEFI:NTFS staging (`wimlib-imagex` + `hivexsh` for the Secure
  Boot bypass) and the `sfdisk` script temp file both use the new
  location. The UEFI:NTFS download cache also prefers `/var/tmp` over
  `/tmp` when `$HOME` is unavailable.
- The invoking user's home is recovered from `PKEXEC_UID` / `SUDO_UID`
  when the worker runs as root, so `$HOME/.cache/rufux` resolves to
  the real user's cache rather than root's.

## 1.5.0 (FAT32 Windows media, Rufus's Windows options, new window)

- **Windows media on FAT32.** Pick FAT32 for a Windows image and Rufux makes
  one plain FAT32 partition (no UEFI:NTFS driver, works with Secure Boot).
  An `install.wim` over 4 GiB is split into `install.swm`, `install2.swm`,
  ... with wimlib, which Setup reads natively. `--fs vfat` on the command
  line, `--split-wim MB` to force a split size. NTFS stays the default.
  This is also a quick test when Windows Setup cannot see a stick: if the
  FAT32 version is found and the NTFS one is not, the layout is the cause.
- **The Windows options are now the ones Rufus has**, in the same dialog:
  create a local account (name prefilled, empty password that must be
  changed at first logon), copy this computer's regional options (language,
  keyboard, time zone), disable BitLocker automatic encryption, disable
  data collection, and a set of "Windows 11 annoyances" switches (Copilot,
  ads, news, classic context menu, Fast Startup). CLI:
  `--wue bypass,nro,privacy,bitlocker,locale,qol,user=NAME`, with
  `--locale`, `--keyboard`, `--timezone`. Options are checked before the
  drive is touched, so a bad account name fails in a second.
- The choices you make in that dialog are remembered.
- **New window details:** modern drop-down lists and check boxes, a
  Settings window (light/dark/system, eject offer), drag and drop of an
  image onto the window, `rufux --gui image.iso`, a compare field in the
  checksum window (paste the published hash: match or not), an "Eject
  drive" button when the write is done, and the full log inside the error
  dialog under "Show Details". A note under the file system explains
  FAT32 versus NTFS for Windows images.
- After a Windows write, the finish dialog says what to try when Setup
  reports a missing media driver (USB-A/USB 2.0 port, `diskpart`,
  `list disk`). README section rewritten to tell the two Setup errors
  apart: "a media driver is missing" (Setup cannot see the stick) and
  "no drives found" (it cannot see the internal disk).
- Changed: `--wue privacy` no longer also disables BitLocker; use the new
  `bitlocker` item (as in Rufus). `--wue all` includes both.
- Tests: `test_wue.sh` covers the new options; `test_fat_split.sh` covers
  the WIM splitting with a real WIM. The FAT32 layout and mount are not
  covered here because the CI sandbox kernel has no vfat driver.

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
  where Rufux puts it, and the architecture (amd64, arm64, x86) is taken
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

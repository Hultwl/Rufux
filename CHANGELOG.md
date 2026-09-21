# Changelog

## 1.7

- The window is now a copy of Rufus's main window: same sections, wording and
  order. It uses the desktop's own widget style, icons and file dialogs, and has
  normal window controls (the AppImage uses the desktop file chooser through
  the portal).
- The Windows options dialog is Rufus's: RAM/Secure Boot/TPM, online account,
  local account, regional options, data collection, BitLocker, QoL tweaks.
- Removed what Rufus does not have: the drivers folder option, the extra
  `BypassCPUCheck` and `BypassStorageCheck` values, the theme setting, the
  eject and checksum-compare extras, and the "no media" hints.
- Windows media is GPT again by default.
- Rufux refuses to finish a GPT write if `sfdisk` cannot name a partition type.
- Like Rufus, ISOHybrid images ask for ISO or DD mode when you press START.

## 1.6.4

- Fixed the partition type of GPT sticks. Rufux wrote a data partition type
  that is not a real GUID, `sfdisk` called it "unknown", and Windows ignores
  such partitions, so Setup saw the drive but no volume on it. It now uses
  `EBD0A0A2-B9E5-4433-87C0-68B6B72699C7`, the Microsoft basic data type.

## 1.6.3 (real MBR bootstrap code, not zeroed)

- Compared byte-for-byte against real Rufus source (`format.c`'s `WriteMBR`).
  Rufus always writes actual x86 bootstrap machine code into the MBR's boot
  area (`mbr_win7.h`, `mbr_rufus.h` - real compiled bytes), and Ventoy's own
  MBR carries its grub-based bootstrap the same way. Rufux, going through
  `sfdisk`, always left that 440-byte region zeroed: structurally a valid
  MBR, but not what any other tool in this space produces.
- Two other explanations for that gap were tested directly against a loop
  device and ruled out: a stale GPT backup header surviving an MBR rewrite,
  and stale bootstrap code from a prior tool (e.g. Ventoy) surviving a
  repartition. `sfdisk --wipe always --wipe-partitions always` already
  clears both correctly.
- Rufux now writes a real bootstrap (the standard, GPL, freely
  redistributable `syslinux mbr.bin` - already an existing Rufux dependency)
  over that region for every MBR/dos-scheme partition table, immediately
  after `sfdisk` builds it. The partition table, disk signature and boot
  signature `sfdisk` just wrote are left untouched; verified byte-for-byte
  on a loop device. UEFI firmware never executes this region, so this
  cannot be why an initial UEFI boot fails, but it closes a real, verified
  structural gap between Rufux's MBR output and every other tool's.

## 1.6.2

- Windows media defaulted to MBR (reverted in 1.7).

## 1.6.1

- `mkfs.ntfs` waits for a consistent partition size, gets the exact sector
  count, and the finished volume is checked so it cannot be larger than its
  partition.

## 1.6.0 (NTFS Windows media fixes)

NTFS sticks booted but Windows Setup stopped with "a media driver your
computer needs is missing", and the install volume showed up as 0 bytes in
diskpart. Four things in the NTFS path contributed:

- **The NTFS volume was described with no disk geometry.** `mkfs.ntfs` only
  fills the BPB fields it can work out for itself, and on a USB stick it
  usually cannot: it printed *"Windows will not be able to boot from this
  device"* and wrote zero for hidden sectors, heads and sectors-per-track.
  Rufux now passes `-p` (partition start), `-H 255` and `-S 63` explicitly,
  the way the Windows formatter does. `--cluster-sectors` is also passed on
  to `mkfs.ntfs` now (`-c`); it used to be accepted and silently dropped.
- **Old filesystem signatures survived repartitioning.** `sfdisk` ran with
  `--wipe always`, which only covers the disk, so it would warn *"Partition
  #1 contains a vfat signature"* and leave the previous volume's superblock
  and its backup copies in place. It now runs with `--wipe-partitions
  always` as well, and the first and last MiB of the Windows data partition
  are zeroed before formatting, which is what Rufus does.
- **An incomplete extraction could pass verification.** The completeness
  check only complained when less than a quarter of the image had been
  written, and the file check looked at `bootmgr` and `sources/boot.wim`
  alone. The threshold is now three quarters, and the check covers every
  file Setup needs to find its media (`setup.exe`, `sources/setup.exe`,
  `boot/bcd`, `efi/microsoft/boot/bcd`, an `efi/boot/boot*.efi` loader),
  matching names case-insensitively so the ISO9660 and UDF spellings both
  resolve.
- **The NTFS dirty flag is cleared** with `ntfsfix -d` after unmounting, so
  Windows treats the stick as installation media rather than as a volume
  awaiting repair.

The window was rebuilt and a few older annoyances went with it:

- **The interface follows Rufus's layout.** Captions sit above their
  controls, partition scheme and target system share a row, as do file
  system and cluster size, and the status is drawn inside the progress
  bar. The hand-drawn check boxes, drop-down arrows and the large custom
  stylesheet are gone: the window is plain Qt widgets and follows the
  desktop theme.
- **The log is part of the window.** Press Log to open a panel under the
  buttons, with Save and Clear. It used to be a separate dialog that had
  to be kept out of the way.
- **Dark mode works in the AppImage.** The system theme was read after the
  widget style had been replaced, which reset the palette to a light one,
  so the answer was always "light". It is now read first, from the Qt
  style hint, then the desktop portal, then gsettings, and a change made
  while Rufux is running is picked up.
- **Splitting install.wim no longer needs scratch space.** For FAT32 media
  the image is loop-mounted and wimlib reads `install.wim` where it lies,
  instead of copying 6.6 GiB to `/tmp` first. The copy is still used for
  images that will not mount, and it now also considers the directory the
  image itself is in.
- **The GTK4 window was removed.** Qt6 is the only interface; without it
  the build is command-line only.

## 1.5.0

- Windows media on FAT32, with `install.wim` split when it is over 4 GiB
  (`--fs vfat`, `--split-wim MB`).
- Windows options: local account, regional options, BitLocker, QoL tweaks.
- Options are checked before the drive is touched.

## 1.4.1

- Drives that report no capacity (empty card-reader slots) are hidden, and a
  zero-size target is refused with a clear message.

## 1.4.0

- The RAM/Secure Boot/TPM bypass is written into `boot.wim` like Rufus does;
  an answer file is only the fallback.

## 1.3.0

- New Qt6 window. `extract` mode now uses one data partition instead of a
  512 MiB one, and single-partition GPT drives get the Windows data type.

## 1.2.7

- Windows install media uses Rufus's layout: NTFS first, a 1 MiB UEFI:NTFS
  partition last. The mount path from `udisksctl` is no longer cut at the
  first dot.

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

- Initial scaffold forked from pbatard/rufus.\n

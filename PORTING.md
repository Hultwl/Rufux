# Porting notes

Rufux is based on [pbatard/rufus](https://github.com/pbatard/rufus) at
commit `2ea79910`. Rufus is about 46,000 lines of Windows-only C. The
upstream sources are kept in the tree for reference and are not compiled.
The Linux code lives in `src/linux/` (core) and `src/gui/` (Qt6 interface, GTK4 kept as a fallback).
Nothing we build includes `windows.h`.

## Reused from upstream

- The MBR and boot-record byte arrays from ms-sys (data only)
- Payloads in `res/`: the UEFI:NTFS image, FreeDOS files, syslinux and
  GRUB binaries, icons, and upstream's translation files

## Rewritten for Linux

Where Rufus calls a Windows API, Rufux runs a standard Linux tool or
system call instead.

| Rufus (Windows) | Rufux (Linux) |
|---|---|
| Device enumeration (SetupDi) | sysfs scan plus `BLKGETSIZE64` |
| Formatting (VDS, `FormatEx`) | `mkfs.vfat`, `mkfs.ntfs`, `mkfs.exfat`, `mkfs.ext4`, `mkudffs` |
| Partitioning (`IOCTL_DISK_*`) | `sfdisk` scripts |
| Raw disk I/O | `open()` with `O_EXCL`, `flock`, `fsync`, `BLKRRPART` |
| Dialogs | Qt6 (GTK4 fallback) |
| Privileges | root check, `pkexec` from the GUI |
| Mounting | `udisksctl` |
| Downloads | `curl` for update checks |

Two rules apply everywhere: never touch a non-removable device without
`--allow-fixed`, and every destructive command prints a plan unless
`--real --yes` is given.

## Windows install media

The stick has two partitions: an NTFS partition with the ISO contents,
then a 1 MiB UEFI:NTFS partition at the very end. This mirrors Rufus. Two
details matter for Windows Setup:

- The data partition comes first.
- The small partition is typed as basic data, not "EFI System". Rufus
  documents that Setup fails when a disk has two ESPs.

Windows User Experience follows Rufus's `wue.c`: the Secure Boot/TPM/RAM
bypass is written into the SYSTEM hive of `boot.wim` (with `wimlib-imagex`
and `hivexsh`), and only if that fails is an answer file with a `windowsPE`
pass used. Other options go to `sources\$OEM$\$$\Panther\unattend.xml`.
`--drivers` copies a folder to `$WinPEDriver$`.

Only UEFI boot works for NTFS sticks. The NTFS boot sector loads more code
from sectors 1-15 of `$Boot`, which `mkfs.ntfs` leaves empty, so BIOS
boot needs a different approach (see [docs/TODO.md](docs/TODO.md)).

## Feature status

| Feature | Status | Notes |
|---|---|---|
| MD5, SHA-1, SHA-256, SHA-512 | Works | OpenSSL |
| Fixed VHD images | Works | Footer checked, payload written; dynamic VHD and VHDX are refused |
| Bad-block scan | Works | Read-only by default; `--write-patterns` writes test patterns |
| FreeDOS stick | Works | DOS boot records, checked byte for byte in tests |
| Windows install media | UEFI only | See above. Not yet confirmed on real hardware after the 1.2.7 layout change |
| Windows 11 check bypass | Works | Answer-file method, same as Rufus |
| ReFS formatting | No | No Linux ReFS formatter exists; refused with a message |
| Windows ISO downloader | No | Microsoft offers no public API; the command explains the manual steps |
| Windows To Go | Not started | Researched in [docs/wintogo-research.md](docs/wintogo-research.md); needs a fast 32 GB+ stick to test |
| Translations | Partial | Our own strings: English, French, Spanish. `res/loc/` is upstream's |

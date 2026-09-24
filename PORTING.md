# Porting notes

Rufux is based on [pbatard/rufus](https://github.com/pbatard/rufus) at commit
`2ea79910`. Rufus is about 46,000 lines of Windows-only C. Its sources are
kept in the tree for reference and are not compiled. The Linux code is in
`src/linux/` (core and command line) and `src/gui/` (the Qt window).

## Taken from Rufus

- The MBR and boot-record byte arrays from ms-sys (data only)
- The files in `res/`: the UEFI:NTFS image, FreeDOS, syslinux and GRUB
  binaries, icons, and Rufus's translation file
- The wording of the window and of the Windows options dialog

## Replaced with Linux tools

| Rufus (Windows) | Rufux (Linux) |
|---|---|
| Device enumeration (SetupDi) | sysfs and `BLKGETSIZE64` |
| Formatting (VDS, `FormatEx`) | `mkfs.vfat` (FAT32/FAT16), `mkfs.ntfs`, `mkfs.exfat`, `mkfs.ext4` (ext2/3/4 by `-t`), `mkudffs` |
| Partitioning (`IOCTL_DISK_*`) | `sfdisk` scripts |
| Raw disk access | `open()` with `O_EXCL`, `fsync`, `BLKRRPART` |
| Dialogs | GTK4 |
| Administrator rights | `pkexec` for the worker, the window stays unprivileged |
| Mounting | `udisksctl` |

Two rules apply everywhere: never touch a non-removable device unless
"List USB Hard Drives" is on, and every command prints a plan unless
`--real --yes` is given.

## Windows install media

NTFS layout: the data partition first, then a 1 MiB UEFI:NTFS partition at the
end (Rufus does the same). The small partition is not typed as an EFI System
partition; Rufus notes that Setup fails on a disk with two of them. FAT32
layout: one partition, and `install.wim` over 4 GiB is split into
`install.swm` parts with `wimlib-imagex split`.

GPT partitions must use the real Microsoft basic data type,
`EBD0A0A2-B9E5-4433-87C0-68B6B72699C7`. Windows ignores partitions of a type it
does not know and shows no volume for the drive. Rufux checks the table after
writing it and refuses one that `sfdisk` cannot name.

The Windows options follow Rufus's `wue.c`. The RAM, Secure Boot and TPM bypass
writes the three `LabConfig` values into the SYSTEM hive of `boot.wim` (with
`wimlib-imagex` and `hivexsh`); only if that fails does it fall back to an
answer file with a `windowsPE` pass. Other options go to
`sources\$OEM$\$$\Panther\unattend.xml`.

## Feature status

| Feature | Status | Notes |
|---|---|---|
| MD5, SHA-1, SHA-256, SHA-512 | Works | OpenSSL |
| Fixed VHD images | Works | Footer checked |
| Dynamic VHD, VHDX, VMDK, QCOW2, VDI | Works | `qemu-img convert` onto the target; images with backing files or foreign extents are refused; consistency-checked first |
| Drive health | Works | `smartctl`; only a failing verdict blocks a write |
| Revoked UEFI bootloaders | Works | DBX hashes and certificates, SBAT, boot manager SVN (`bootcheck.c`, from `hash.c`); the `pe256ssp` list Rufus downloads is not used |
| Legacy BIOS boot, Windows media | Works (MBR) | GRUB 2 in the MBR gap runs `ntldr /bootmgr` on FAT32/NTFS; GPT stays UEFI-only |
| Bad-block scan | Works | Read-only by default, `--write-patterns` writes test patterns |
| FreeDOS | Works | DOS boot records |
| Windows install media | Works | UEFI boot; see the limits in the README |
| ReFS | No | No Linux formatter exists |
| Windows ISO download | Works | The protocol of Fido (session, SKU lookup, link request). Microsoft documents none of it, so it can break; tested here against a stand-in server only |
| Windows To Go | No | |
| Language button | No | |

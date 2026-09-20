<p align="center">
  <img src="https://raw.githubusercontent.com/Hultwl/Rufux/main/res/icons/rufus-128.png" width="128" alt="Rufux logo">
</p>

<h1 align="center">Rufux</h1>

<p align="center">
  Make bootable USB sticks on Linux, the way Rufus does on Windows.
</p>

<p align="center">
  <a href="https://github.com/Hultwl/Rufux/releases/latest"><img src="https://img.shields.io/github/v/release/Hultwl/Rufux?style=flat-square&label=release" alt="Latest release"></a>
  <a href="https://github.com/Hultwl/Rufux/actions/workflows/rufux.yml"><img src="https://github.com/Hultwl/Rufux/actions/workflows/rufux.yml/badge.svg" alt="CI build"></a>
  <a href="https://aur.archlinux.org/packages/rufux-git"><img src="https://img.shields.io/aur/version/rufux-git?style=flat-square&label=AUR" alt="AUR version"></a>
  <a href="https://github.com/Hultwl/Rufux/blob/main/LICENSE.txt"><img src="https://img.shields.io/github/license/Hultwl/Rufux?style=flat-square" alt="License: GPLv3"></a>
  <img src="https://img.shields.io/github/languages/top/Hultwl/Rufux?style=flat-square" alt="Top language: C">
</p>

<p align="center">
  <a href="https://github.com/Hultwl/Rufux/releases/latest/download/Rufux-x86_64.AppImage"><strong>⬇ Download AppImage</strong></a>
</p>

<p align="center">
  <img src="screenshots/rufux-main.png" width="520" alt="Rufux main window">
</p>

---

Rufux is a Linux port of [Rufus](https://github.com/pbatard/rufus). It
burns ISOs to USB sticks, and unlike `dd` it can also partition, format,
extract, verify, and build Windows install media.

The graphical interface is built with Qt6. Most of the code was written with an AI assistant and then tested on real
sticks. It is young software: read the limits below before trusting it
with anything you care about, and try `--dry-run` first.

## What it does

- **Write an image** in raw mode (`dd`-style, with read-back verification)
  or in file mode (partition, format, extract the ISO, install a
  bootloader).
- **Windows install media**, laid out like Rufus does it: an NTFS
  partition holding the ISO contents, plus a 1 MiB UEFI:NTFS partition at
  the end so UEFI machines can boot from NTFS. Optionally adds an
  `autounattend.xml` that skips the Windows 11 hardware checks.
- **FreeDOS sticks** with real DOS boot records.
- **Extras:** MD5/SHA-1/SHA-256/SHA-512 checksums, fixed VHD images,
  persistence partitions, bad-block scans, Secure Boot status.
- **Safety:** commands only print a plan unless you pass `--real --yes`
  and run as root. Fixed disks, mounted targets, and a target equal to
  the source are refused.

## Limits you should know about

- Windows sticks made in file mode boot on **UEFI** machines only. Legacy
  BIOS boot from NTFS needs a Windows-written boot loader that Linux
  formatting tools don't produce.
- If Windows Setup says a media driver is missing, please open an issue
  with the log. Version 1.2.7 changed the stick layout to match Rufus
  because of that error, but it could not be tested against a real
  Windows install.
- Not supported: ReFS, the built-in Windows ISO downloader, Windows To Go.
  [PORTING.md](PORTING.md) explains each.

## Install

**AppImage:** download it from the
[latest release](https://github.com/Hultwl/Rufux/releases/latest), then
`chmod +x Rufux-x86_64.AppImage` and run it.

**Arch:** `paru -S rufux-git`

**From source:**

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
sudo cmake --install build
```

The host needs `dosfstools`, `ntfs-3g`/`ntfsprogs`, `exfatprogs`,
`e2fsprogs`, `util-linux`, `syslinux`, `udisks2`, and `p7zip` or
`libarchive`. See [packaging/README.md](packaging/README.md) for the full
list per distribution. Some tests skip themselves when a tool or root
access is missing, and the DOS boot-record test needs `gawk`.

## Usage

```sh
rufux list                                   # removable drives
rufux probe image.iso --detail               # label, size, bootable?
rufux write image.iso /dev/sdX --dry-run     # show the plan
sudo rufux write image.iso /dev/sdX --real --verify --yes

# Windows install media, with the Windows 11 checks disabled:
sudo rufux create Win11.iso /dev/sdX --mode windows --scheme gpt \
     --wue bypass --real --yes

rufux --gui                                  # graphical interface
```

Run `rufux` with no arguments for every option, or `man rufux` after
installing.

## More

- [PORTING.md](PORTING.md): what was kept from Rufus, what was rewritten
- [CHANGELOG.md](CHANGELOG.md): release notes
- [docs/TODO.md](docs/TODO.md): known problems and planned work
- [tests/HW_MATRIX.md](tests/HW_MATRIX.md): hardware test checklist

Bug reports with the log attached help most (the GUI has a Save button).

## Origin and license

Port of [pbatard/rufus](https://github.com/pbatard/rufus) by Pete Batard.
GPLv3, like upstream. Formerly called Lufus; renamed to avoid confusion
with [Hogjects/Lufus](https://github.com/Hogjects/Lufus), an unrelated
project.

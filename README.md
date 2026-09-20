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

The interface is Qt6. Most of the code was written with the help of an AI
assistant and then tested on real sticks. It is young software: read the
limits below before trusting it with anything you care about, and try
`--dry-run` first.

## What it does

- **Write an image** in raw mode (`dd`-style, with read-back verification)
  or in file mode (partition, format, extract the ISO, install a
  bootloader).
- **Windows install media**, laid out like Rufus does it: either NTFS plus
  a 1 MiB UEFI:NTFS partition (any image size), or one FAT32 partition
  (Secure Boot friendly, `install.wim` split when needed). Rufus's Windows
  options are there too: skip the hardware checks, local account,
  regional options, no BitLocker, no data collection, and more.
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
- The NTFS path was reworked in 1.6.0 after Windows Setup reported a
  missing media driver: the volume was being written without disk
  geometry, and old filesystem signatures survived repartitioning. The
  fixes are verified against the formatting tools, not against a real
  Windows install. If Setup still cannot find the drive, open an issue
  with the log.
- Not supported: ReFS, the built-in Windows ISO downloader, Windows To Go.
  [PORTING.md](PORTING.md) explains each.

## When Windows Setup cannot find something

There are two different errors that both end on "Install driver to show
hardware", and they need different fixes.

**"A media driver your computer needs is missing"** means Setup cannot see
the USB stick itself, so it cannot find the install files.
1. Use a plain USB-A port, ideally USB 2.0. Avoid USB-C/Thunderbolt ports,
   hubs and adapters. Windows PE lacks drivers for some of these, and a
   stick that boots fine can then disappear once Windows takes over.
2. At the error, press Shift+F10, type `diskpart`, then `list disk`. Your
   stick should be listed. (`list volume` also shows an empty card-reader
   slot as "Removable, 0 B, No Media"; that is not your stick.)
3. If the disk is listed but "Offline": `select disk N`, `online disk`.
4. Write the same ISO again with the **FAT32** file system and try again.
   If the FAT32 stick is found and the NTFS one is not, the layout was the
   problem; if neither is found, it is the port or the USB controller.
5. Still nothing: add the chipset/USB controller driver of your laptop
   with `--drivers` (below), or try another stick or port.

**No drives listed at "Where do you want to install Windows?"** means Setup
cannot see the internal disk, common with Intel RST/VMD (RAID) mode.
1. In the firmware settings switch the storage mode from "RAID / Intel
   RST" to **AHCI**, install, and switch back if you need it.
2. Or download the storage driver from your laptop maker ("Intel Rapid
   Storage Technology" or "VMD", F6 version), unzip it, and pass the folder:
   `rufux create Win11.iso /dev/sdX --mode windows --drivers ~/vmd --real --yes`
   (or the drivers folder in the GUI's Windows options).

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

Building needs Qt6 Widgets; without it you get a command-line-only binary.
At runtime the host needs `dosfstools`, `ntfs-3g`/`ntfsprogs`,
`exfatprogs`, `e2fsprogs`, `util-linux`, `syslinux`, `udisks2`, and
`p7zip` or `libarchive`. Splitting a large `install.wim` for FAT32 needs
`wimlib` (wimlib-imagex), and the Windows 11 checks bypass also needs
`hivex`. [packaging/README.md](packaging/README.md) has the full list per
distribution. Some tests skip themselves when a tool or root access is
missing, and the DOS boot-record test needs `gawk`.

## Usage

```sh
rufux list                                   # removable drives
rufux probe image.iso --detail               # label, size, bootable?
rufux write image.iso /dev/sdX --dry-run     # show the plan
sudo rufux write image.iso /dev/sdX --real --verify --yes

# Windows install media, with the Windows 11 checks disabled:
sudo rufux create Win11.iso /dev/sdX --mode windows --scheme gpt \
     --wue bypass --real --yes

# The same as FAT32 (install.wim is split if needed), with a local account:
sudo rufux create Win11.iso /dev/sdX --mode windows --fs vfat \
     --wue bypass,nro,user=Sam --real --yes

rufux --gui                                  # graphical interface
```

Run `rufux` with no arguments for every option, or `man rufux` after
installing.

## More

- [PORTING.md](PORTING.md): what was kept from Rufus, what was rewritten
- [CHANGELOG.md](CHANGELOG.md): release notes
- [docs/TODO.md](docs/TODO.md): known problems and planned work
- [tests/HW_MATRIX.md](tests/HW_MATRIX.md): hardware test checklist

Bug reports with the log attached help most: press Log in the window and
then Save log.

## Origin and license

Port of [pbatard/rufus](https://github.com/pbatard/rufus) by Pete Batard.
GPLv3, like upstream. Formerly called Lufus; renamed to avoid confusion
with [Hogjects/Lufus](https://github.com/Hogjects/Lufus), an unrelated
project.

# Rufux for macOS (Apple Silicon `arm64`)

Native macOS port of **Rufux 2.3.0** — the open-source bootable USB formatting and OS installation utility modeled 1:1 on Rufus. All macOS source code, vendored libraries, build scripts, automated tests, and packaged artifacts live inside this self-contained `macos/` directory without modifying the upstream Linux repository root.

## Highlights

- **Self-Contained `Rufux.app` & `.dmg` (`macos/dist/`):**
  - `macos/dist/Rufux.app` (ad-hoc signed inside-out, zero external `/opt/homebrew` library links)
  - `macos/dist/Rufux-2.3.0-macos-arm64.dmg` (compressed UDZO disk image with `Rufux.app` + `/Applications` symlink, ad-hoc signed)
- **All 8 Filesystems Supported Without Kernel Extensions:**
  - **FAT16 / FAT32 / exFAT / UDF:** Native macOS formatters (`/sbin/newfs_msdos`, `/sbin/newfs_exfat`, `/sbin/newfs_udf`).
  - **NTFS:** Vendored Tuxera `ntfs-3g 2026.9.28` (`mkntfs`, `ntfsfix`, and direct in-process `libntfs-3g` streaming population in `ntfs_populate.c`).
  - **ext2 / ext3 / ext4:** Bundled `e2fsprogs` (`mke2fs -d` direct userspace directory population and `e2fsck`) plus `casper-rw` Linux persistence file creation.
- **Kext-less Read-Write Mounting (`rufux-mountd`):**
  - Mounts both **NTFS** and **ext2/ext3/ext4** volumes read-write via a bundled localhost NFSv3 + MOUNTv3 loopback daemon (`src/mountd_nfs.c`) linked against `libntfs-3g` and `libext2fs`, paired with macOS's built-in `/sbin/mount_nfs` — zero `.kext`s, zero `macFUSE`, and zero Recovery Mode security changes required.
- **Windows 10/11 & Legacy BIOS / UEFI Support:**
  - Pure-C protective/DOS MBR and CRC32-verified primary/backup GPT partition writer (`src/partition.c`).
  - Windows `UEFI:NTFS` dual-partition layout (`res/uefi/uefi-ntfs.img` with GPT attribute bit 63).
  - Windows User Experience (`--wue`) automation: `boot.wim` `SYSTEM` registry hive `LabConfig` patching (`BypassTPMCheck`, `BypassSecureBootCheck`, `BypassRAMCheck`) via bundled `wimlib-imagex` + `hivexsh`, `autounattend.xml` generation, and `> 4 GiB` WIM splitting (`--split-wim`).
  - Legacy BIOS boot installation (`src/biosboot.c`) using bundled `i686-elf-grub-mkimage` and `i386-pc` GRUB2 modules (`boot.img` + blocklist-patched `core.img` in MBR gap or GPT `BIOS boot` partition).
- **Boot-Disk Armor (`src/platform_macos.c` & `src/device.c`):**
  - Resolves the full IOKit parent lineage of `/` (including synthesized APFS containers and physical stores) and unconditionally excludes and refuses `/dev/disk0` and the active boot disk even when `--allow-fixed` is passed.
- **Native Tauri v2 GUI (`macos/gui-tauri/`):**
  - 1:1 Rufus WebView UI paired with native macOS administrator privilege escalation (`osascript ... with administrator privileges`) streaming real-time progress over a `0700` per-job FIFO session directory.

## Directory Layout

```text
macos/
├── CMakeLists.txt              # Builds `rufux`, `rufux-mountd`, and C/shell test targets
├── README.md                   # This file
├── scripts/
│   ├── install-deps.sh         # Installs Homebrew build deps + builds vendored Tuxera ntfs-3g
│   ├── make-icns.sh            # Generates Rufux.icns from ../res/icons/rufux-512.png
│   ├── bundle-app.sh           # Assembles Rufux.app, relocates Mach-O dylibs, ad-hoc signs inside-out
│   └── build-dmg.sh            # End-to-end CMake + Cargo build -> bundle -> UDZO DMG -> ad-hoc sign
├── src/                        # Native macOS C11 CLI backend & NFSv3 mount daemon
├── gui-tauri/                  # macOS Tauri v2 desktop application crate
├── tests/                      # Rootless CTest unit, integration, and clean-room bundle tests
└── dist/                       # Output artifacts: Rufux.app and Rufux-2.3.0-macos-arm64.dmg
```

## Building from Source

### 1. Install Dependencies & Build Vendored `ntfs-3g`

```bash
bash macos/scripts/install-deps.sh
```

### 2. Build `Rufux.app` and `Rufux-2.3.0-macos-arm64.dmg`

```bash
bash macos/scripts/build-dmg.sh
```

This produces:
- `macos/dist/Rufux.app`
- `macos/dist/Rufux-2.3.0-macos-arm64.dmg`

### 3. Verify Bundle Self-Containment & Ad-Hoc Signatures

```bash
codesign --verify --deep --strict --verbose=2 macos/dist/Rufux.app
codesign --verify --verbose=2 macos/dist/Rufux-2.3.0-macos-arm64.dmg
hdiutil verify macos/dist/Rufux-2.3.0-macos-arm64.dmg
```

### 4. Run the Automated CTest Suite

```bash
ctest --test-dir macos/build --output-on-failure
```

The test suite runs 8 automated suites (`ctest_core`, `test_core`, `ctest_device_writer`, `ctest_partition_boot`, `ctest_filesystems`, `test_mount_rw`, `test_create_e2e`, and `test_bundle_dmg`), including a clean-room `env -i` test that copies `Rufux.app` to a path with spaces and verifies device listing, NTFS/ext4 formatting and read-write NFSv3 mounting, and full Windows 11 GPT+NTFS+GRUB2+WUE image creation with zero external Homebrew environment.

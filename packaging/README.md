# Packaging Rufux

Three ways out the door. The AppImage is the one most people want.

## AppImage (recommended)

Attached to every GitHub release, built by
`.github/workflows/appimage.yml` on Ubuntu 24.04:

```sh
chmod +x Rufux-x86_64.AppImage
./Rufux-x86_64.AppImage   # opens the GUI; CLI via --help etc.
```

The AppImage ships its own helpers (all `mkfs.*`, `sfdisk`,
`partprobe`, `bsdtar`, `7z`, `wimlib-imagex`, `hivexsh`, `syslinux`
plus its MBR data, `udisksctl`, `curl`) bundled under
`AppDir/usr/bin`, with payloads, locales and MBR data resolved
exe-relative — see `src/linux/exec.c` (`rufux_exe_dir`) and
`.github/workflows/appimage.yml` ("Bundle helper tools"). The host
only needs the `udisks2` daemon running (mounts) and `grub` if you
use GRUB mode (module tree too big to bundle).
No Flatpak — sandboxes and raw disks don't mix, tried that, walked away.

## Arch Linux (AUR)

Two packages, both pushed automatically — no manual AUR uploads:

- `rufux` (stable): refreshed on every GitHub release from
  `packaging/PKGBUILD`, which builds the release tag.
- `rufux-git`: refreshed on every push to `main`; its version
  follows git, so `paru -S rufux-git` always rebuilds the latest
  commit.

```sh
paru -S rufux        # stable release
paru -S rufux-git    # latest main
```

The automation lives in `.github/workflows/aur.yml`. It needs one
secret, `AUR_SSH_PRIVATE_KEY` — without it the jobs skip quietly:

1. Generate a deploy key (or reuse an existing AUR key):
   `ssh-keygen -t ed25519 -f ~/.ssh/rufux-aur -N ""`
2. Add the public half to your aur.archlinux.org account.
3. Add the private half as a repo secret named
   `AUR_SSH_PRIVATE_KEY` (Settings → Secrets and variables →
   Actions).

Manual fallback (same thing the workflow does):
`packaging/aur/publish.sh` (needs an Arch box with the AUR key).

## From source (any distro)

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
sudo cmake --install build
```

Runtime deps: `qt6-base`, `udisks2`, `util-linux` (sfdisk), `openssl`,
`dosfstools`, `ntfsprogs`, `exfatprogs`, `e2fsprogs`,
`libarchive` (bsdtar), `p7zip` (7z — required for UDF/Windows
ISOs, which bsdtar under-extracts, and for unpacking the UEFI:NTFS
ESP payload), `syslinux`, `curl`.

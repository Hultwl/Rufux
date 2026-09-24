#!/bin/bash
# Copies just the GRUB 2 i386-pc files that Rufux's Windows BIOS boot needs (boot.img,
# kernel.img and the modules its embedded script uses, with their dependencies) so the
# AppImage carries about 320 KB instead of the ~3 MB module tree.
# Usage: prune-grub.sh SRC_DIR DST_DIR      e.g. /usr/lib/grub/i386-pc AppDir/usr/lib/grub/i386-pc
set -e
src=${1:?source dir}; dst=${2:?destination dir}
need="biosdisk part_msdos part_gpt fat ntfs exfat ntldr search search_fs_file echo sleep boot"   # keep in sync with biosboot.c
[ -f "$src/moddep.lst" ] && [ -f "$src/boot.img" ] && [ -f "$src/kernel.img" ] || { echo "not a GRUB i386-pc directory: $src" >&2; exit 1; }
mkdir -p "$dst"
mods=$(awk -v want="$need" '
  BEGIN { n = split(want, w, " "); for (i = 1; i <= n; i++) todo[w[i]] = 1 }
  { sub(":", "", $1); dep[$1] = $0 }
  END {
    do { changed = 0
      for (m in todo) if (!(m in done)) {
        done[m] = 1; changed = 1
        n = split(dep[m], f, " ")
        for (i = 2; i <= n; i++) todo[f[i]] = 1
      }
    } while (changed)
    for (m in done) print m
  }' "$src/moddep.lst")
for m in $mods; do
  [ -f "$src/$m.mod" ] || { echo "missing module $m.mod in $src" >&2; exit 1; }
  cp "$src/$m.mod" "$dst/"
done
# grub-mkimage also reads these images (found by trial: it names the first one missing)
for f in boot.img kernel.img diskboot.img lzma_decompress.img moddep.lst; do
  [ -f "$src/$f" ] || { echo "missing $f in $src" >&2; exit 1; }
  cp "$src/$f" "$dst/"
done
echo "grub: $(echo $mods | wc -w) modules, $(du -sk "$dst" | cut -f1) KB in $dst"

#!/bin/bash
# Test-branch AppImage assembly for the Tauri GUI (local runs, Arch paths).
# Mirrors .github/workflows/appimage.yml (Ubuntu paths there).
# Usage: ./packaging/appimage-tauri.sh   (run from repo root)
set -e
ROOT="$PWD"
[ -f CMakeLists.txt ] && [ -d gui-tauri ] || { echo "run from repo root"; exit 1; }

echo "== backend =="
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr >/dev/null
cmake --build build -j"$(nproc)" --target rufux 2>&1 | tail -1

echo "== frontend =="
cargo build --release --manifest-path gui-tauri/Cargo.toml 2>&1 | tail -1
ls -la gui-tauri/target/release/rufux-gui

echo "== AppDir =="
rm -rf AppDir
DESTDIR="$ROOT/AppDir" cmake --install build >/dev/null
install -m755 gui-tauri/target/release/rufux-gui AppDir/usr/bin/rufux-gui.bin
install -m755 packaging/rufux-gui.sh AppDir/usr/bin/rufux-gui
ln -sf usr/bin/rufux AppDir/AppRun

echo "== helper tools =="
mkdir -p AppDir/usr/bin
deploy_tool() {
  name="$1"; shift
  for c in "$@"; do
    case "$c" in
      */*) [ -x "$c" ] && p="$c" || continue ;;
      *) p="$(command -v "$c" 2>/dev/null)" || continue ;;
    esac
    p="$(readlink -f "$p")"
    if [ "$(head -c 4 "$p" | od -An -tx1 | tr -d ' \n')" = "7f454c46" ]; then
      echo "-e $p" >> "$ROOT/.rufux-tools"
      base="$(basename "$p")"
      [ "$base" = "$name" ] || ln -sf "$base" "AppDir/usr/bin/$name"
      echo "$name -> $p"
      return 0
    fi
  done
  echo "no ELF binary for: $name ($*)"; exit 1
}
: > "$ROOT/.rufux-tools"
for t in mkfs.vfat mkfs.ntfs ntfsfix mkfs.exfat mkfs.ext4 mkfs.udf sfdisk partprobe bsdtar wimlib-imagex hivexsh syslinux udisksctl curl; do
  deploy_tool "$t" "$t"
done
deploy_tool 7z 7zz 7za 7zr /usr/lib/7zip/7z
[ -f /usr/lib/7zip/7z.so ] && cp /usr/lib/7zip/7z.so AppDir/usr/bin/7z.so
for t in smartctl qemu-img grub-mkimage grub-bios-setup; do
  deploy_tool "$t" "$t"
done
packaging/prune-grub.sh /usr/lib/grub/i386-pc AppDir/usr/lib/grub/i386-pc
mkdir -p AppDir/usr/share/syslinux
cp /usr/lib/syslinux/bios/mbr.bin /usr/lib/syslinux/bios/gptmbr.bin AppDir/usr/share/syslinux/
ls AppDir/usr/share/syslinux/

echo "== linuxdeploy =="
[ -x linuxdeploy-x86_64.AppImage ] || wget -q https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
chmod +x linuxdeploy-x86_64.AppImage
export APPIMAGE_EXTRACT_AND_RUN=1
./linuxdeploy-x86_64.AppImage --appimage-extract-and-run \
  --appdir AppDir \
  -e AppDir/usr/bin/rufux \
  -e AppDir/usr/bin/rufux-gui.bin \
  $(tr '\n' ' ' < "$ROOT/.rufux-tools") \
  --exclude-library libwebkit2gtk-4.1.so.0 \
  --exclude-library libjavascriptcoregtk-4.1.so.0 \
  --exclude-library libsoup-3.0.so.0 \
  --exclude-library libgtk-3.so.0 \
  --exclude-library libgdk-3.so.0 \
  --exclude-library libavif.so.16 \
  --exclude-library libglycin-2.so.0 \
  --exclude-library libaom.so.3 \
  --exclude-library libdav1d.so.7 \
  --exclude-library librav1e.so.0.8 \
  --exclude-library libSvtAv1Enc.so.4 \
  --exclude-library libjxl.so.0.12 \
  --exclude-library libjxl_cms.so.0.12 \
  --exclude-library libsharpyuv.so.0 \
  --exclude-library libyuv.so \
  --exclude-library libgstreamer-1.0.so.0 \
  --exclude-library libgstallocators-1.0.so.0 \
  --exclude-library libgstapp-1.0.so.0 \
  --exclude-library libgstaudio-1.0.so.0 \
  --exclude-library libgstbase-1.0.so.0 \
  --exclude-library libgstfft-1.0.so.0 \
  --exclude-library libgstgl-1.0.so.0 \
  --exclude-library libgstpbutils-1.0.so.0 \
  --exclude-library libgsttag-1.0.so.0 \
  --exclude-library libgstvideo-1.0.so.0 \
  --exclude-library libhyphen.so.0 \
  --exclude-library libxslt.so.1 \
  --exclude-library libenchant-2.so.2 \
  --exclude-library libmanette-0.2.so.0 \
  --exclude-library libsecret-1.so.0 \
  -d AppDir/usr/share/applications/io.github.hultwl.rufux.desktop \
  -i AppDir/usr/share/icons/hicolor/128x128/apps/io.github.hultwl.rufux.png \
  --output appimage
ls -la ./*.AppImage

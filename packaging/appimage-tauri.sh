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
install -m755 gui-tauri/target/release/rufux-gui AppDir/usr/bin/rufux-gui
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
deploy_tool 7z 7zz 7za 7zr 7z /usr/lib/7zip/7z /usr/lib/p7zip/7z /usr/lib/p7zip/7za
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
# WebKitGTK ships whole (self-containment): helpers are spawned by baked
# absolute path, so copy them in for the LD_PRELOAD exec shim.
WKDIR="$(dirname "$(find /usr/lib -name WebKitWebProcess -path "*webkit*" 2>/dev/null | head -1)")"
[ -n "$WKDIR" ] || { echo "WebKit helpers not found"; exit 1; }
mkdir -p AppDir/usr/lib/webkit2gtk-4.1
cp "$WKDIR"/WebKitWebProcess "$WKDIR"/WebKitNetworkProcess AppDir/usr/lib/webkit2gtk-4.1/
[ -f "$WKDIR/WebKitGPUProcess" ] && cp "$WKDIR"/WebKitGPUProcess AppDir/usr/lib/webkit2gtk-4.1/ || true
cp -r "$WKDIR"/injected-bundle AppDir/usr/lib/webkit2gtk-4.1/
patchelf --set-rpath '$ORIGIN/..' AppDir/usr/lib/webkit2gtk-4.1/WebKitWebProcess AppDir/usr/lib/webkit2gtk-4.1/WebKitNetworkProcess
[ -f AppDir/usr/lib/webkit2gtk-4.1/WebKitGPUProcess ] && patchelf --set-rpath '$ORIGIN/..' AppDir/usr/lib/webkit2gtk-4.1/WebKitGPUProcess || true
./linuxdeploy-x86_64.AppImage --appimage-extract-and-run \
  --appdir AppDir \
  -e AppDir/usr/bin/rufux \
  -e AppDir/usr/bin/rufux-gui \
  $(tr '\n' ' ' < "$ROOT/.rufux-tools") \
  -d AppDir/usr/share/applications/io.github.hultwl.rufux.desktop \
  -i AppDir/usr/share/icons/hicolor/128x128/apps/io.github.hultwl.rufux.png \
  --output appimage
ls -la ./*.AppImage

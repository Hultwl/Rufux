#!/usr/bin/env bash
set -euo pipefail

MACOS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT_DIR="$(cd "${MACOS_DIR}/.." && pwd)"
DIST_DIR="${MACOS_DIR}/dist"
APP_DIR="${DIST_DIR}/Rufux.app"
CONTENTS_DIR="${APP_DIR}/Contents"
MACOS_BIN_DIR="${CONTENTS_DIR}/MacOS"
RES_DIR="${CONTENTS_DIR}/Resources"
RES_BIN_DIR="${RES_DIR}/bin"
RES_LIB_DIR="${RES_DIR}/lib"
RES_GRUB_DIR="${RES_LIB_DIR}/grub/i386-pc"
RES_SHARE_DIR="${RES_DIR}/share/rufux"

echo "==> [bundle-app] Ensuring CMake CLI & mountd binaries are built..."
if [ ! -x "${MACOS_DIR}/build/rufux" ] || [ ! -x "${MACOS_DIR}/build/rufux-mountd" ]; then
  cmake -S "${MACOS_DIR}" -B "${MACOS_DIR}/build" -DCMAKE_BUILD_TYPE=Release
  cmake --build "${MACOS_DIR}/build" -j"$(sysctl -n hw.ncpu)"
fi

echo "==> [bundle-app] Ensuring Rufux.icns is generated..."
if [ ! -f "${MACOS_DIR}/gui-tauri/icons/Rufux.icns" ]; then
  bash "${MACOS_DIR}/scripts/make-icns.sh"
fi

echo "==> [bundle-app] Ensuring Tauri GUI release binary is built..."
if [ ! -x "${MACOS_DIR}/gui-tauri/target/release/rufux-gui" ]; then
  export PATH="${HOME}/.cargo/bin${SUDO_USER:+:/Users/${SUDO_USER}/.cargo/bin}:/opt/homebrew/bin:${PATH}"
  (cd "${MACOS_DIR}/gui-tauri" && cargo build --release)
fi

echo "==> [bundle-app] Assembling ${APP_DIR} directory hierarchy..."
rm -rf "${APP_DIR}"
mkdir -p "${MACOS_BIN_DIR}" "${RES_BIN_DIR}" "${RES_LIB_DIR}" "${RES_GRUB_DIR}" "${RES_SHARE_DIR}"

cat > "${CONTENTS_DIR}/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>
  <string>en</string>
  <key>CFBundleDisplayName</key>
  <string>Rufux</string>
  <key>CFBundleExecutable</key>
  <string>rufux-gui</string>
  <key>CFBundleIconFile</key>
  <string>Rufux.icns</string>
  <key>CFBundleIdentifier</key>
  <string>io.github.hultwl.rufux</string>
  <key>CFBundleInfoDictionaryVersion</key>
  <string>6.0</string>
  <key>CFBundleName</key>
  <string>Rufux</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>3.0</string>
  <key>CFBundleVersion</key>
  <string>3.0</string>
  <key>LSMinimumSystemVersion</key>
  <string>13.0</string>
  <key>NSHighResolutionCapable</key>
  <true/>
</dict>
</plist>
PLIST

cp -f "${MACOS_DIR}/gui-tauri/icons/Rufux.icns" "${RES_DIR}/Rufux.icns"
chmod 0644 "${RES_DIR}/Rufux.icns" "${CONTENTS_DIR}/Info.plist"

echo "==> [bundle-app] Copying main executables and helper tools..."
cp -f "${MACOS_DIR}/gui-tauri/target/release/rufux-gui" "${MACOS_BIN_DIR}/rufux-gui"
cp -f "${MACOS_DIR}/build/rufux" "${MACOS_BIN_DIR}/rufux"
chmod 0755 "${MACOS_BIN_DIR}/rufux-gui" "${MACOS_BIN_DIR}/rufux"

cp -f "${MACOS_DIR}/build/rufux" "${RES_BIN_DIR}/rufux"
cp -f "${MACOS_DIR}/build/rufux-mountd" "${RES_BIN_DIR}/rufux-mountd"
cp -f "${MACOS_DIR}/vendor/ntfs-3g/sbin/mkntfs" "${RES_BIN_DIR}/mkntfs"
cp -f "${MACOS_DIR}/vendor/ntfs-3g/bin/ntfsfix" "${RES_BIN_DIR}/ntfsfix"
cp -f "/opt/homebrew/opt/e2fsprogs/sbin/mke2fs" "${RES_BIN_DIR}/mke2fs"
cp -f "/opt/homebrew/opt/e2fsprogs/sbin/e2fsck" "${RES_BIN_DIR}/e2fsck"

copy_hb_tool() {
  local name="$1"
  local src=""
  for cand in "/opt/homebrew/bin/${name}" "/opt/homebrew/sbin/${name}"; do
    if [ -x "${cand}" ]; then
      src="${cand}"
      break
    fi
  done
  if [ -z "${src}" ]; then
    echo "ERROR: Required Homebrew tool '${name}' not found" >&2
    exit 1
  fi
  cp -L "${src}" "${RES_BIN_DIR}/${name}"
  chmod 0755 "${RES_BIN_DIR}/${name}"
}

copy_hb_tool "7zz"
copy_hb_tool "wimlib-imagex"
copy_hb_tool "hivexsh"
copy_hb_tool "smartctl"
copy_hb_tool "qemu-img"
copy_hb_tool "i686-elf-grub-mkimage"

chmod 0755 "${RES_BIN_DIR}"/*

ln -sf "mkntfs" "${RES_BIN_DIR}/mkfs.ntfs"
ln -sf "mke2fs" "${RES_BIN_DIR}/mkfs.ext2"
ln -sf "mke2fs" "${RES_BIN_DIR}/mkfs.ext3"
ln -sf "mke2fs" "${RES_BIN_DIR}/mkfs.ext4"
ln -sf "7zz" "${RES_BIN_DIR}/7z"
ln -sf "i686-elf-grub-mkimage" "${RES_BIN_DIR}/grub-mkimage"

echo "==> [bundle-app] Staging GRUB i386-pc modules and runtime assets..."
GRUB_SRC=""
for cand in \
  "/opt/homebrew/opt/i686-elf-grub/lib/i686-elf/grub/i386-pc" \
  "/opt/homebrew/lib/i686-elf/grub/i386-pc"; do
  if [ -d "${cand}" ] && [ -f "${cand}/boot.img" ]; then
    GRUB_SRC="${cand}"
    break
  fi
done
if [ -z "${GRUB_SRC}" ]; then
  echo "ERROR: GRUB i386-pc module directory not found" >&2
  exit 1
fi

for f in "${GRUB_SRC}"/*.img "${GRUB_SRC}"/*.mod "${GRUB_SRC}"/*.lst; do
  if [ -f "${f}" ]; then
    cp -f "${f}" "${RES_GRUB_DIR}/$(basename "${f}")"
  fi
done
chmod 0644 "${RES_GRUB_DIR}"/*

for sub in freedos uefi dbx icons syslinux mbr grub grub2; do
  if [ -d "${ROOT_DIR}/res/${sub}" ]; then
    mkdir -p "${RES_SHARE_DIR}/${sub}"
    cp -R "${ROOT_DIR}/res/${sub}/." "${RES_SHARE_DIR}/${sub}/"
  fi
done
ln -sf "freedos" "${RES_SHARE_DIR}/fdos"
if [ -f "${RES_SHARE_DIR}/uefi/uefi-ntfs.img" ]; then
  cp -f "${RES_SHARE_DIR}/uefi/uefi-ntfs.img" "${RES_SHARE_DIR}/uefi-ntfs.img"
fi

echo "==> [bundle-app] Recursively bundling & relocating Mach-O .dylib closure..."
/usr/bin/python3 - "${APP_DIR}" "${MACOS_DIR}" <<'PY'
import glob
import os
import shutil
import subprocess
import sys

app_dir = sys.argv[1]
macos_dir = sys.argv[2]
contents_dir = os.path.join(app_dir, "Contents")
macos_bin_dir = os.path.join(contents_dir, "MacOS")
res_bin_dir = os.path.join(contents_dir, "Resources", "bin")
res_lib_dir = os.path.join(contents_dir, "Resources", "lib")

search_dirs = [
    os.path.join(macos_dir, "vendor", "ntfs-3g", "lib"),
    "/opt/homebrew/lib",
    "/opt/homebrew/opt/e2fsprogs/lib",
    "/opt/homebrew/opt/openssl@3/lib",
    "/opt/homebrew/opt/gettext/lib",
    "/opt/homebrew/opt/readline/lib",
    "/opt/homebrew/opt/xz/lib",
    "/opt/homebrew/opt/zstd/lib",
    "/opt/homebrew/opt/libssh/lib",
    "/opt/homebrew/opt/glib/lib",
    "/opt/homebrew/opt/gnutls/lib",
    "/opt/homebrew/opt/wimlib/lib",
    "/opt/homebrew/opt/hivex/lib",
]
for d in glob.glob("/opt/homebrew/opt/*/lib"):
    if d not in search_dirs:
        search_dirs.append(d)

def is_macho(path):
    if os.path.islink(path) or not os.path.isfile(path):
        return False
    out = subprocess.check_output(["/usr/bin/file", "-b", path], text=True)
    return "Mach-O" in out

def otool_deps(path):
    out = subprocess.check_output(["/usr/bin/otool", "-L", path], text=True)
    lines = out.splitlines()[1:]
    res = []
    for l in lines:
        parts = l.strip().split()
        if parts:
            res.append(parts[0])
    return res

def otool_rpaths(path):
    out = subprocess.check_output(["/usr/bin/otool", "-l", path], text=True)
    lines = out.splitlines()
    rpaths = []
    for i, l in enumerate(lines):
        if "cmd LC_RPATH" in l and i + 2 < len(lines):
            pline = lines[i + 2].strip()
            if pline.startswith("path "):
                rpaths.append(pline.split()[1])
    return rpaths

def is_system_dep(dep):
    return dep.startswith("/System/") or dep.startswith("/usr/lib/")

def resolve_dep(dep, origin_dir):
    if os.path.exists(dep):
        return os.path.realpath(dep)
    bname = os.path.basename(dep)
    for d in ([origin_dir] if origin_dir else []) + search_dirs:
        cand = os.path.join(d, bname)
        if os.path.exists(cand):
            return os.path.realpath(cand)
    raise RuntimeError(f"Cannot resolve Mach-O dependency '{dep}' (origin='{origin_dir}')")

exec_files = []
for d in [macos_bin_dir, res_bin_dir]:
    for name in sorted(os.listdir(d)):
        p = os.path.join(d, name)
        if is_macho(p):
            exec_files.append(p)

queue = [(p, None, False) for p in exec_files]
seen_targets = set()
copied_dylibs = {}  # basename -> local path in res_lib_dir

while queue:
    target, origin_dir, is_dylib = queue.pop(0)
    if target in seen_targets:
        continue
    seen_targets.add(target)

    deps = otool_deps(target)
    if is_dylib and deps:
        # First line of otool -L on a dylib is its own LC_ID_DYLIB
        deps = deps[1:]

    for dep in deps:
        if is_system_dep(dep):
            continue
        bname = os.path.basename(dep)
        dst_dylib = os.path.join(res_lib_dir, bname)
        if bname not in copied_dylibs:
            real_src = resolve_dep(dep, origin_dir)
            shutil.copy2(real_src, dst_dylib)
            os.chmod(dst_dylib, 0o755)
            copied_dylibs[bname] = dst_dylib
            queue.append((dst_dylib, os.path.dirname(real_src), True))

def strip_host_rpaths(path):
    for rp in otool_rpaths(path):
        if any(tok in rp for tok in ("homebrew", "/usr/local", "/Users/", "vendor")):
            subprocess.check_call(["/usr/bin/install_name_tool", "-delete_rpath", rp, path])

# 1. Rewrite LC_ID_DYLIB and inter-dylib dependencies in Contents/Resources/lib/*.dylib
for bname, dylib_path in sorted(copied_dylibs.items()):
    os.chmod(dylib_path, 0o755)
    subprocess.check_call(["/usr/bin/install_name_tool", "-id", f"@loader_path/{bname}", dylib_path])
    deps = otool_deps(dylib_path)[1:]
    for dep in deps:
        if is_system_dep(dep):
            continue
        dep_base = os.path.basename(dep)
        new_dep = f"@loader_path/{dep_base}"
        if dep != new_dep:
            subprocess.check_call(["/usr/bin/install_name_tool", "-change", dep, new_dep, dylib_path])
    strip_host_rpaths(dylib_path)

# 2. Rewrite dependencies in Contents/Resources/bin/* -> @executable_path/../lib/<dep>
for name in sorted(os.listdir(res_bin_dir)):
    p = os.path.join(res_bin_dir, name)
    if not is_macho(p):
        continue
    os.chmod(p, 0o755)
    for dep in otool_deps(p):
        if is_system_dep(dep):
            continue
        dep_base = os.path.basename(dep)
        new_dep = f"@executable_path/../lib/{dep_base}"
        if dep != new_dep:
            subprocess.check_call(["/usr/bin/install_name_tool", "-change", dep, new_dep, p])
    strip_host_rpaths(p)

# 3. Rewrite dependencies in Contents/MacOS/* -> @executable_path/../Resources/lib/<dep>
for name in sorted(os.listdir(macos_bin_dir)):
    p = os.path.join(macos_bin_dir, name)
    if not is_macho(p):
        continue
    os.chmod(p, 0o755)
    for dep in otool_deps(p):
        if is_system_dep(dep):
            continue
        dep_base = os.path.basename(dep)
        new_dep = f"@executable_path/../Resources/lib/{dep_base}"
        if dep != new_dep:
            subprocess.check_call(["/usr/bin/install_name_tool", "-change", dep, new_dep, p])
    strip_host_rpaths(p)

print(f"Relocated {len(copied_dylibs)} bundled dylibs and {len(exec_files)} executables.")
PY

echo "==> [bundle-app] Verifying zero external library references before signing..."
while IFS= read -r -d '' f; do
  if ! /usr/bin/file -b "${f}" | grep -q "Mach-O"; then
    continue
  fi
  deps="$(/usr/bin/otool -L "${f}" | tail -n +2 | awk '{print $1}')"
  while IFS= read -r dep; do
    [ -n "${dep}" ] || continue
    case "${dep}" in
      /System/Library/*|/usr/lib/*|@executable_path/*|@loader_path/*|@rpath/*) ;;
      *)
        echo "ERROR: Unrelocated dependency '${dep}' in '${f}'" >&2
        exit 1
        ;;
    esac
  done <<< "${deps}"
done < <(find "${APP_DIR}" -type f -print0)

echo "==> [bundle-app] Stripping xattrs and ad-hoc signing inside-out..."
/usr/bin/xattr -cr "${APP_DIR}"

for dylib in "${RES_LIB_DIR}"/*.dylib; do
  if [ -f "${dylib}" ] && [ ! -L "${dylib}" ]; then
    /usr/bin/codesign --force --sign - --timestamp=none "${dylib}"
  fi
done

for bin in "${RES_BIN_DIR}"/*; do
  if [ -f "${bin}" ] && [ ! -L "${bin}" ] && /usr/bin/file -b "${bin}" | grep -q "Mach-O"; then
    /usr/bin/codesign --force --sign - --timestamp=none "${bin}"
  fi
done

for bin in "${MACOS_BIN_DIR}"/*; do
  if [ -f "${bin}" ] && [ ! -L "${bin}" ] && /usr/bin/file -b "${bin}" | grep -q "Mach-O"; then
    /usr/bin/codesign --force --sign - --timestamp=none "${bin}"
  fi
done

/usr/bin/codesign --force --sign - --timestamp=none "${APP_DIR}"

echo "==> [bundle-app] Verifying ad-hoc signature..."
/usr/bin/codesign --verify --deep --strict --verbose=2 "${APP_DIR}"
echo "==> [bundle-app] Successfully created and verified ${APP_DIR}"

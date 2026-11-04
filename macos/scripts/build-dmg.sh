#!/usr/bin/env bash
set -euo pipefail

MACOS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST_DIR="${MACOS_DIR}/dist"
APP_DIR="${DIST_DIR}/Rufux.app"
DMG_PATH="${DIST_DIR}/Rufux-3.0-macos-arm64.dmg"

export PATH="${HOME}/.cargo/bin${SUDO_USER:+:/Users/${SUDO_USER}/.cargo/bin}:/opt/homebrew/bin:/opt/homebrew/sbin:${PATH}"

STAGE_DIR="$(mktemp -d /tmp/rufux-dmg-stage-XXXXXX)"
cleanup() {
  rm -rf "${STAGE_DIR}"
}
trap cleanup EXIT

echo "==> [1/6] Building CMake C11 backend (rufux & rufux-mountd)..."
cmake -S "${MACOS_DIR}" -B "${MACOS_DIR}/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "${MACOS_DIR}/build" -j"$(sysctl -n hw.ncpu)"

echo "==> [2/6] Generating Rufux.icns iconset..."
bash "${MACOS_DIR}/scripts/make-icns.sh"

echo "==> [3/6] Building Tauri v2 GUI release binary (rufux-gui)..."
(cd "${MACOS_DIR}/gui-tauri" && cargo build --release)

echo "==> [4/6] Assembling, relocating, and ad-hoc signing Rufux.app..."
bash "${MACOS_DIR}/scripts/bundle-app.sh"

echo "==> [5/6] Creating compressed UDZO disk image ${DMG_PATH}..."
mkdir -p "${DIST_DIR}"
rm -f "${DMG_PATH}"
/usr/bin/ditto "${APP_DIR}" "${STAGE_DIR}/Rufux.app"
ln -s /Applications "${STAGE_DIR}/Applications"

/usr/bin/hdiutil create \
  -volname "Rufux 3.0" \
  -srcfolder "${STAGE_DIR}" \
  -ov \
  -format UDZO \
  "${DMG_PATH}"

echo "==> [6/6] Ad-hoc signing and verifying ${DMG_PATH}..."
/usr/bin/xattr -cr "${DMG_PATH}" || true
/usr/bin/codesign --force --sign - --timestamp=none "${DMG_PATH}"
/usr/bin/codesign --verify --verbose=2 "${DMG_PATH}"
/usr/bin/hdiutil verify "${DMG_PATH}"

echo "==> Build complete:"
ls -lh "${DMG_PATH}"

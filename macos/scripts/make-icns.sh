#!/usr/bin/env bash
set -euo pipefail

MACOS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_PNG="${MACOS_DIR}/res/icons/rufux-macos-512.png"
if [ ! -f "${SRC_PNG}" ]; then
  SRC_PNG="${MACOS_DIR}/../res/icons/rufux-512.png"
fi
ICONS_DIR="${MACOS_DIR}/gui-tauri/icons"

if [ ! -f "${SRC_PNG}" ]; then
  echo "ERROR: Source icon not found at ${SRC_PNG}" >&2
  exit 1
fi

mkdir -p "${ICONS_DIR}"

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/rufux-iconset.XXXXXX")"
trap 'rm -rf "${TMP_DIR}"' EXIT
ICONSET="${TMP_DIR}/Rufux.iconset"
mkdir -p "${ICONSET}"

/usr/bin/sips -s format png -z 16 16     "${SRC_PNG}" --out "${ICONSET}/icon_16x16.png" >/dev/null
/usr/bin/sips -s format png -z 32 32     "${SRC_PNG}" --out "${ICONSET}/icon_16x16@2x.png" >/dev/null
/usr/bin/sips -s format png -z 32 32     "${SRC_PNG}" --out "${ICONSET}/icon_32x32.png" >/dev/null
/usr/bin/sips -s format png -z 64 64     "${SRC_PNG}" --out "${ICONSET}/icon_32x32@2x.png" >/dev/null
/usr/bin/sips -s format png -z 128 128   "${SRC_PNG}" --out "${ICONSET}/icon_128x128.png" >/dev/null
/usr/bin/sips -s format png -z 256 256   "${SRC_PNG}" --out "${ICONSET}/icon_128x128@2x.png" >/dev/null
/usr/bin/sips -s format png -z 256 256   "${SRC_PNG}" --out "${ICONSET}/icon_256x256.png" >/dev/null
/usr/bin/sips -s format png -z 512 512   "${SRC_PNG}" --out "${ICONSET}/icon_256x256@2x.png" >/dev/null
/usr/bin/sips -s format png -z 512 512   "${SRC_PNG}" --out "${ICONSET}/icon_512x512.png" >/dev/null
/usr/bin/sips -s format png -z 1024 1024 "${SRC_PNG}" --out "${ICONSET}/icon_512x512@2x.png" >/dev/null

/usr/bin/iconutil -c icns "${ICONSET}" -o "${ICONS_DIR}/Rufux.icns"

cp "${ICONSET}/icon_32x32.png" "${ICONS_DIR}/32x32.png"
cp "${ICONSET}/icon_128x128.png" "${ICONS_DIR}/128x128.png"
cp "${ICONSET}/icon_128x128@2x.png" "${ICONS_DIR}/128x128@2x.png"
cp "${SRC_PNG}" "${ICONS_DIR}/icon.png"

echo "==> Generated ${ICONS_DIR}/Rufux.icns and PNG icons from ${SRC_PNG}"

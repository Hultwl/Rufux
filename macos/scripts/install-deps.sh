#!/usr/bin/env bash
set -euo pipefail

MACOS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"

FORMULAE=(
  rust
  sevenzip
  e2fsprogs
  hivex
  smartmontools
  qemu
  i686-elf-grub
  wimlib
  # GNU build system for ntfs-3g's autogen.sh (autoreconf + autopoint)
  autoconf
  automake
  libtool
  gettext
)

echo "==> Checking Homebrew dependencies..."
# Brew refuses root. When the script is sudoed, drop to the invoking user;
# otherwise (CI runners, direct runs) use the current user. Override with
# MACOS_BREW_USER if the default does not fit.
BREW_USER="${MACOS_BREW_USER:-${SUDO_USER:-$(id -un)}}"
echo "==> Using brew as: ${BREW_USER}"
INSTALLED="$(sudo -H -u "${BREW_USER}" /opt/homebrew/bin/brew list --formula)"
MISSING=()
for pkg in "${FORMULAE[@]}"; do
  if ! printf '%s\n' "${INSTALLED}" | grep -qx "${pkg}"; then
    MISSING+=("${pkg}")
  fi
done

if [ "${#MISSING[@]}" -gt 0 ]; then
  echo "==> Installing missing Homebrew formulae as ${BREW_USER}: ${MISSING[*]}"
  sudo -H -u "${BREW_USER}" env HOMEBREW_ASK= HOMEBREW_NO_AUTO_UPDATE=1 /opt/homebrew/bin/brew install "${MISSING[@]}" </dev/null
else
  echo "==> All required Homebrew formulae already installed."
fi

NTFS3G_VER="2026.9.28"
NTFS3G_URL="https://github.com/tuxera/ntfs-3g/archive/refs/tags/${NTFS3G_VER}.tar.gz"
NTFS3G_SHA256="8a75293f07bf40df5701d296de32a4cb0e46d6ae135abb4f41b438df9868a390"

VENDOR_DIR="${MACOS_DIR}/vendor"
PREFIX="${VENDOR_DIR}/ntfs-3g"
SRC_DIR="${VENDOR_DIR}/src"
TARBALL="${SRC_DIR}/ntfs-3g-${NTFS3G_VER}.tar.gz"
BUILD_SRC="${SRC_DIR}/ntfs-3g-${NTFS3G_VER}"

if [ "${FORCE_REBUILD:-0}" != "1" ] && \
   [ -x "${PREFIX}/sbin/mkntfs" ] && \
   [ -x "${PREFIX}/bin/ntfsfix" ] && \
   [ -f "${PREFIX}/lib/libntfs-3g.dylib" ]; then
  echo "==> Vendor ntfs-3g ${NTFS3G_VER} already built in ${PREFIX}"
else
  echo "==> Building Tuxera ntfs-3g ${NTFS3G_VER} into ${PREFIX}..."
  rm -rf "${PREFIX}"
  mkdir -p "${SRC_DIR}" "${PREFIX}"

  if [ ! -f "${TARBALL}" ]; then
    /usr/bin/curl -fL --retry 3 -o "${TARBALL}" "${NTFS3G_URL}"
  fi

  ACTUAL_SHA="$(shasum -a 256 "${TARBALL}" | awk '{print $1}')"
  if [ "${ACTUAL_SHA}" != "${NTFS3G_SHA256}" ]; then
    echo "ERROR: SHA-256 mismatch for ${TARBALL}" >&2
    echo "  expected: ${NTFS3G_SHA256}" >&2
    echo "  actual:   ${ACTUAL_SHA}" >&2
    rm -f "${TARBALL}"
    exit 1
  fi

  rm -rf "${BUILD_SRC}"
  tar -xzf "${TARBALL}" -C "${SRC_DIR}"

  pushd "${BUILD_SRC}" >/dev/null

  # Patch hardcoded $(DESTDIR)/sbin before and after autogen if needed.
  find . -name 'Makefile.am' -exec perl -pi -e 's#\$\(DESTDIR\)/sbin#\$\(DESTDIR\)\$\(sbindir\)#g' {} +

  if [ ! -x ./configure ]; then
    mkdir -p m4
    if [ ! -f /opt/homebrew/share/aclocal/libgcrypt.m4 ]; then
      printf 'AC_DEFUN([AM_PATH_LIBGCRYPT], [$3])\n' > m4/gcrypt-stub.m4
    fi
    ACLOCAL_PATH="/opt/homebrew/share/aclocal" LIBTOOLIZE="/opt/homebrew/bin/glibtoolize" ./autogen.sh
  fi

  find . -name 'Makefile.in' -exec perl -pi -e 's#\$\(DESTDIR\)/sbin#\$\(DESTDIR\)\$\(sbindir\)#g' {} +

  export MACOSX_DEPLOYMENT_TARGET="13.0"
  PKG_CONFIG_PATH="/opt/homebrew/opt/openssl@3/lib/pkgconfig:${PKG_CONFIG_PATH:-}" \
  CFLAGS="-O2 -arch arm64 -mmacosx-version-min=13.0" \
  LDFLAGS="-arch arm64 -mmacosx-version-min=13.0" \
  ./configure \
    --prefix="${PREFIX}" \
    --exec-prefix="${PREFIX}" \
    --disable-ntfs-3g \
    --enable-ntfsprogs \
    --enable-extras \
    --disable-ldconfig \
    --disable-static \
    --enable-shared

  make -j"$(sysctl -n hw.ncpu)" install
  popd >/dev/null
fi

echo "==> Verifying installed toolchains and vendor ntfs-3g..."
"${PREFIX}/sbin/mkntfs" --version
"${PREFIX}/bin/ntfsfix" --version
/opt/homebrew/opt/e2fsprogs/sbin/mke2fs -V
/opt/homebrew/bin/7zz | head -n 4
/opt/homebrew/bin/cargo --version
/opt/homebrew/bin/i686-elf-grub-mkimage --version
echo "==> All dependencies installed and verified."

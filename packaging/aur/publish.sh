#!/bin/bash
# Manual fallback for the AUR publish that CI does on its own
# (.github/workflows/aur.yml: rufux-git on every main push, stable
# rufux on every release). Run on Arch, with your AUR SSH key set up.
# The AUR only shows what is pushed to aur.archlinux.org: updating
# GitHub does not update it, which is why it can lag behind (or show
# an old version).
#   packaging/aur/publish.sh            # rufux-git
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
pkg=rufux-git
work=$(mktemp -d)
git clone "ssh://aur@aur.archlinux.org/$pkg.git" "$work/$pkg"
cp "$here/$pkg/PKGBUILD" "$work/$pkg/PKGBUILD"
cd "$work/$pkg"
makepkg -o --noconfirm                 # fetch sources so pkgver() resolves
makepkg --printsrcinfo > .SRCINFO      # regenerates pkgver from git tags
git add PKGBUILD .SRCINFO
git commit -m "Update to $(grep -m1 '^\s*pkgver' .SRCINFO | awk '{print $3}')"
git push origin HEAD:master
echo "Done. Check https://aur.archlinux.org/packages/$pkg"

#!/usr/bin/env bash
# Native editor-only CI bootstrap: xlings has no Intel macOS release.
set -euo pipefail
install_root="${1:?provide an isolated install directory}"
mkdir -p "$install_root"
for version in 0.10.4 0.12.5; do
  case "$version" in
    0.10.4) expected=c1405071127b59dbdefc31d9c52e9a5c36db67dcef6dcf83e898aada1f3f778e ;;
    0.12.5) expected=81f4518622cb059b450ee2e498c6a1082a222f6bd89589de5bbcf0c6a68aa3fd ;;
  esac
  archive="$install_root/nvim-$version.tar.gz"
  curl -fsSL --retry 3 --retry-all-errors --retry-delay 2 \
    "https://github.com/neovim/neovim/releases/download/v$version/nvim-macos-x86_64.tar.gz" \
    -o "$archive"
  actual="$(shasum -a 256 "$archive")"
  [ "${actual%% *}" = "$expected" ] || { echo "Neovim checksum mismatch" >&2; exit 1; }
  mkdir -p "$install_root/$version"
  tar -xzf "$archive" --strip-components=1 -C "$install_root/$version"
  "$install_root/$version/bin/nvim" --version
done

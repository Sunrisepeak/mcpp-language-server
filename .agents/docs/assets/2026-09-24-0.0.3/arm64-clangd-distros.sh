#!/usr/bin/env bash
# Run the official LLVM arm64 clangd against each distro's own glibc/libstdc++/zlib, via qemu -L.
set -u
S=$(cd "$(dirname "$0")" && pwd)
R=$S/roots; mkdir -p "$R"
A=https://mirrors.aliyun.com

latest() { # $1 = listing url, $2 = regex; prints the last (highest) match
  curl -s -m 30 "$1" | sed "s/&#43;/+/g" | grep -oE "$2" | sed 's/+/%2B/g; s/:/%3A/g' | sort -uV | tail -1
}

ubuntu() { # $1 = name, $2 = series, $3 = tarball
  local d=$R/$1; [ -d "$d" ] && return; mkdir -p "$d"
  curl -s -m 300 "$A/ubuntu-cdimage/ubuntu-base/releases/$2/release/$3" | tar -xz -C "$d" 2>/dev/null
}

debs() { # $1 = name, then "pool-dir regex" pairs
  local d=$R/$1; [ -d "$d" ] && return; mkdir -p "$d"; shift
  while [ $# -gt 0 ]; do
    local f; f=$(latest "$A/debian/pool/main/$1/" "$2")
    [ -n "$f" ] && curl -s -m 120 -o "$d/p.deb" "$A/debian/pool/main/$1/$f" && dpkg-deb -x "$d/p.deb" "$d" && echo "  $f" >> "$d/.pkgs"
    shift 2
  done; rm -f "$d/p.deb"
}

rpms() { # $1 = name, $2 = Packages base url, then "letter regex" pairs
  local d=$R/$1; [ -d "$d" ] && return; mkdir -p "$d"; local base=$2; shift 2
  while [ $# -gt 0 ]; do
    local f; f=$(latest "$base/$1/" "$2")
    [ -n "$f" ] && curl -s -m 120 -o "$d/p.rpm" "$base/$1/$f" && (cd "$d" && rpm2cpio p.rpm | cpio -idm --quiet) && echo "  $f" >> "$d/.pkgs"
    shift 2
  done; rm -f "$d/p.rpm"
}

ubuntu ubuntu-20.04 20.04 ubuntu-base-20.04.5-base-arm64.tar.gz &
ubuntu ubuntu-22.04 22.04 ubuntu-base-22.04.5-base-arm64.tar.gz &
ubuntu ubuntu-24.04 24.04 ubuntu-base-24.04.5-base-arm64.tar.gz &
debs debian-11 g/glibc 'libc6_2\.31-[^"]*_arm64\.deb' g/gcc-10 'libstdc(\+|%2B)(\+|%2B)6_10[^"]*_arm64\.deb' g/gcc-10 'libgcc-s1_10[^"]*_arm64\.deb' z/zlib 'zlib1g_1\.2\.11[^"]*_arm64\.deb' &
debs debian-12 g/glibc 'libc6_2\.36-[^"]*_arm64\.deb' g/gcc-12 'libstdc(\+|%2B)(\+|%2B)6_12[^"]*_arm64\.deb' g/gcc-12 'libgcc-s1_12[^"]*_arm64\.deb' z/zlib 'zlib1g_1\.2\.13[^"]*_arm64\.deb' &
for v in 8 9; do
  rpms rocky-$v "$A/rockylinux/$v/BaseOS/aarch64/os/Packages" g 'glibc-2[^"]*\.aarch64\.rpm' l 'libstdc(\+|%2B)(\+|%2B)-[0-9][^"]*\.aarch64\.rpm' l 'libgcc-[0-9][^"]*\.aarch64\.rpm' z 'zlib-1[^"]*\.aarch64\.rpm' &
done
rpms openeuler-22.03 "$A/openeuler/openEuler-22.03-LTS-SP4/OS/aarch64/Packages" . 'glibc-2[^"]*\.aarch64\.rpm' . 'libstdc(\+|%2B)(\+|%2B)-[0-9][^"]*\.aarch64\.rpm' . 'libgcc-[0-9][^"]*\.aarch64\.rpm' . 'zlib-1[^"]*\.aarch64\.rpm' &
rpms openeuler-24.03 "$A/openeuler/openEuler-24.03-LTS/OS/aarch64/Packages" . 'glibc-2[^"]*\.aarch64\.rpm' . 'libstdc(\+|%2B)(\+|%2B)-[0-9][^"]*\.aarch64\.rpm' . 'libgcc-[0-9][^"]*\.aarch64\.rpm' . 'zlib-1[^"]*\.aarch64\.rpm' &
wait

for d in "$R"/*; do
  name=$(basename "$d")
  ld=$(find "$d" -name 'ld-linux-aarch64.so.1' | head -1)
  libs=$(find "$d" \( -name 'libc.so.6' -o -name 'libstdc++.so.6' -o -name 'libz.so.1' -o -name 'libgcc_s.so.1' \) -printf '%h\n' | sort -u | tr '\n' ':')
  glibc=$(find "$d" -name 'libc.so.6' | head -1 | xargs -r strings 2>/dev/null | grep -oE '^GLIBC_2\.[0-9]+' | sort -uV | tail -1)
  cxx=$(find "$d" -name 'libstdc++.so.6*' -type f | head -1 | xargs -r strings 2>/dev/null | grep -oE '^GLIBCXX_3\.4\.[0-9]+' | sort -uV | tail -1)
  if [ -z "$ld" ]; then echo "$name | (no loader downloaded)"; continue; fi
  out=$(timeout 60 qemu-aarch64 -L "$d" "$ld" --library-path "$libs" "$S/c/clangd" --version 2>&1 | head -2 | tr '\n' ' ')
  echo "$name | $glibc $cxx | $out"
done

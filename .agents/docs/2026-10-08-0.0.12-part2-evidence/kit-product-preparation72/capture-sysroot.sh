set -eu
mkdir -p /out/sysroot/usr /out/sysroot-licenses
cp -a /usr/include /out/sysroot/usr/
cp /usr/share/doc/libc6-dev/copyright /out/sysroot-licenses/libc6-dev-copyright.txt
cp /usr/share/doc/linux-libc-dev/copyright /out/sysroot-licenses/linux-libc-dev-copyright.txt
dpkg-query -W libc6-dev linux-libc-dev > /out/sysroot-packages.txt

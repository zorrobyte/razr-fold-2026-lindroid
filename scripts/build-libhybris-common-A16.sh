#!/bin/bash
# Runs INSIDE the Lindroid container. Installs Halium android-headers, then
# rebuilds libhybris-common with the __stack_chk_fail/__*_chk hooks patch.
set -e
export LC_ALL=C
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export HOME=/root

unset TMPDIR

echo "=== 1. install android-headers (report Android 11 = API 30, matching stock) ==="
cd /root/ahdr
tar xzf ah-halium.tgz
# libhybris configure reads ANDROID_VERSION_MAJOR from android-version.h to gate
# HAS_ANDROID_10_0_0 -> -DWANT_LINKER_Q. The real android-headers-30 reported 11.
sed -i -e 's/#define ANDROID_VERSION_MAJOR .*/#define ANDROID_VERSION_MAJOR 11/' \
       -e 's/#define ANDROID_VERSION_MINOR .*/#define ANDROID_VERSION_MINOR 0/' \
       -e 's/#define ANDROID_VERSION_PATCH .*/#define ANDROID_VERSION_PATCH 0/' android-version.h
grep ANDROID_VERSION_MAJOR android-version.h
make install PREFIX=/usr >/tmp/ahinst.log 2>&1 && echo "headers installed" || { echo "INSTALL FAIL"; tail -15 /tmp/ahinst.log; exit 1; }
pkg-config --exists android-headers && echo "pkg-config sees android-headers ($(pkg-config --modversion android-headers))" || { echo "PKGCONFIG MISS"; exit 1; }

echo "=== 2. confirm hooks patch present ==="
grep -c "_hybris_hook___stack_chk_fail" /root/libhybris-src/hybris/common/hooks.c

echo "=== 3. backup current lib ==="
CUR=/usr/lib/aarch64-linux-gnu/libhybris-common.so.1
if [ -f "$CUR" ] && [ ! -f "$CUR.orig" ]; then cp -a "$CUR" "$CUR.orig"; echo "backed up $CUR.orig"; else echo "backup exists/skip"; fi

echo "=== 4. configure ==="
cd /root/libhybris-src/hybris
make distclean >/dev/null 2>&1 || true
./configure --build=aarch64-linux-gnu --host=aarch64-linux-gnu \
   --prefix=/usr --libdir=/usr/lib/aarch64-linux-gnu \
   --enable-arch=arm64 --disable-trace >/tmp/hc.log 2>&1 \
   && echo "configure OK" || { echo "CONFIGURE FAIL"; grep -iE "error|android-headers|not found" /tmp/hc.log | tail; exit 1; }
grep -i "android-headers" /tmp/hc.log | tail -2

echo "=== 5. build libhybris-common ==="
make -j"$(nproc)" -C common >/tmp/hm.log 2>&1 \
   && echo "BUILD OK" || { echo "BUILD FAIL"; grep -iE "error:|undefined|No such|fatal" /tmp/hm.log | tail -25; exit 1; }

echo "=== 6. locate built lib ==="
BUILT=$(find /root/libhybris-src/hybris/common -name "libhybris-common.so*" -type f 2>/dev/null | head -1)
echo "built: $BUILT"; ls -la "$BUILT"
echo "DONE-BUILD"

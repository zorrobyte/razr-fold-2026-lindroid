#!/bin/bash
# Build Lindroid userspace (liblxc, lxc tools, perspectived + AIDL stubs) with Android NDK
# Runs in WSL. Layout: /root/lindroid/{ndk.zip,lxc,vendor_lindroid,out}
set -euo pipefail

LD=/root/lindroid
NDKVER=android-ndk-r27c
API=33
TC=$LD/$NDKVER/toolchains/llvm/prebuilt/linux-x86_64
CC=$TC/bin/aarch64-linux-android$API-clang
CXX=$TC/bin/aarch64-linux-android$API-clang++
OUT=$LD/out
mkdir -p $OUT/{obj,lib,bin,gen}

step() { echo; echo "==== $1 ===="; }

# ---- 0) unpack NDK ----
if [ ! -d "$LD/$NDKVER" ]; then
  step "unpack NDK"
  cd $LD && unzip -q ndk.zip
fi
$CC --version | head -1

# ---- 1) liblxc ----
LXC=$LD/lxc
INCS="-I$LXC/src/lxc -I$LXC/src -I$LXC/src/lxc/cgroups -I$LXC/src/lxc/storage -I$LXC/src/include"
CFLAGS="-Wall -std=gnu11 -fPIC -O2 -Wno-error -DIN_LIBLXC=1 -Wno-unused-command-line-argument"

LIBLXC_SRCS="
src/lxc/cgroups/cgfsng.c src/lxc/cgroups/cgroup.c src/lxc/cgroups/cgroup2_devices.c
src/lxc/cgroups/cgroup_utils.c src/lxc/lsm/lsm.c src/lxc/lsm/nop.c
src/lxc/storage/btrfs.c src/lxc/storage/dir.c src/lxc/storage/loop.c src/lxc/storage/lvm.c
src/lxc/storage/nbd.c src/lxc/storage/overlay.c src/lxc/storage/rbd.c src/lxc/storage/rsync.c
src/lxc/storage/storage.c src/lxc/storage/storage_utils.c src/lxc/storage/zfs.c
src/lxc/af_unix.c src/lxc/attach.c src/lxc/caps.c src/lxc/commands.c src/lxc/commands_utils.c
src/lxc/conf.c src/lxc/confile.c src/lxc/confile_utils.c src/lxc/criu.c src/lxc/error.c
src/lxc/execute.c src/lxc/file_utils.c src/lxc/freezer.c src/lxc/idmap_utils.c
src/lxc/initutils.c src/lxc/log.c src/lxc/lxccontainer.c src/lxc/lxclock.c src/lxc/mainloop.c
src/lxc/monitor.c src/lxc/mount_utils.c src/lxc/namespace.c src/lxc/network.c src/lxc/nl.c
src/lxc/parse.c src/lxc/process_utils.c src/lxc/rexec.c src/lxc/ringbuf.c src/lxc/rtnl.c
src/lxc/start.c src/lxc/state.c src/lxc/string_utils.c src/lxc/sync.c src/lxc/terminal.c
src/lxc/utils.c src/lxc/uuid.c src/include/lxcmntent.c src/include/netns_ifaddrs.c
"

step "compile liblxc"
OBJS=""
for s in $LIBLXC_SRCS; do
  o=$OUT/obj/$(echo $s | tr / _).o
  OBJS="$OBJS $o"
  if [ ! -f $o ] || [ $LXC/$s -nt $o ]; then
    echo "CC $s"
    $CC $CFLAGS $INCS -c $LXC/$s -o $o
  fi
done
step "link liblxc.so"
$CC -shared -o $OUT/lib/liblxc.so $OBJS -ldl -lm

# ---- 2) lxc tools ----
TOOLS="attach autostart cgroup checkpoint config console copy create destroy device execute freeze info ls monitor snapshot start stop top unfreeze unshare"
step "build lxc tools"
$CC $CFLAGS $INCS -c $LXC/src/lxc/tools/arguments.c -o $OUT/obj/arguments.o
for t in $TOOLS; do
  src=$LXC/src/lxc/tools/lxc_$t.c
  [ -f $src ] || { echo "skip lxc_$t (no src)"; continue; }
  echo "LD lxc_$t"
  $CC $CFLAGS $INCS $src $OUT/obj/arguments.o -o $OUT/bin/lxc_$t -L$OUT/lib -llxc
done

# ---- 3) AIDL NDK stubs for IPerspective ----
step "AIDL gen"
AIDL=$LD/build-tools/aidl
if [ ! -x $AIDL ]; then
  cd $LD && curl -sL -o build-tools.zip "https://dl.google.com/android/repository/build-tools_r34-linux.zip" && \
  unzip -q -o build-tools.zip -d bt-extract && mkdir -p build-tools && \
  find bt-extract -name aidl -type f -exec cp {} build-tools/aidl \; && chmod +x build-tools/aidl
fi
# aidl needs the build-tools libc++.so on its library path
export LD_LIBRARY_PATH="$(dirname "$(find $LD/bt-extract -name 'libc++.so' | head -1)"):${LD_LIBRARY_PATH:-}"
VND=$LD/vendor_lindroid
GEN=$OUT/gen
rm -rf $GEN/perspective && mkdir -p $GEN/perspective
$AIDL --lang=ndk --structured \
  -I $VND/interfaces/perspective \
  -h $GEN/perspective -o $GEN/perspective \
  $VND/interfaces/perspective/vendor/lindroid/perspective/IPerspective.aidl

# ---- 4) perspectived ----
step "build perspectived"
CXXFLAGS="-std=c++20 -fPIC -O2 -DLOG_TAG=\"perspectived\""
PINC="-I$LD/shim -I$GEN/perspective -I$VND/perspectived -I$LXC/src -I$LXC/src/lxc -I$LXC/src/include"
$CXX $CXXFLAGS $PINC -c $GEN/perspective/vendor/lindroid/perspective/IPerspective.cpp -o $OUT/obj/IPerspective.o
$CXX $CXXFLAGS $PINC -c $VND/perspectived/PerspectiveService.cpp -o $OUT/obj/PerspectiveService.o
$CXX $CXXFLAGS $PINC -c $VND/perspectived/LXCContainerManager.cpp -o $OUT/obj/LXCContainerManager.o
# AServiceManager_addService / ABinderProcess_joinThreadPool are systemapi:
# present in the device's real libbinder_ndk.so (DT_NEEDED via -lbinder_ndk) but
# stripped from the NDK stub, so resolve them at runtime.
# -static-libstdc++ : Android has no libc++_shared.so; link the NDK C++ runtime in.
$CXX -static-libstdc++ -o $OUT/bin/perspectived $OUT/obj/IPerspective.o $OUT/obj/PerspectiveService.o $OUT/obj/LXCContainerManager.o \
  -L$OUT/lib -llxc -lbinder_ndk -llog \
  -Wl,--allow-shlib-undefined -Wl,--unresolved-symbols=ignore-all

step "DONE"
ls -la $OUT/bin $OUT/lib

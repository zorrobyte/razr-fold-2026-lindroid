# native-turnip — libhybris-free GPU for Lindroid on stock Android 16

Source for the native **Turnip/freedreno-on-KGSL** approach. Full writeup:
[`../docs/native-turnip.md`](../docs/native-turnip.md).

The idea: render the container's GPU work with **upstream Mesa (Turnip Vulkan / freedreno GL)
talking straight to `/dev/kgsl-3d0`** — no libhybris, no Android EGL blob — and get frames to
SurfaceFlinger via a native present bridge. Proven: turnip enumerates + renders on the Adreno 829
over KGSL, GLES 3.2 works via zink→turnip, and Android accepts our natively-allocated frames.

## Files

| File | What |
|---|---|
| `m2.c` | Vulkan/turnip render into a linear DRM-modifier image on exportable dma-buf memory, readback-verified, fd exported. The dma-buf proof. Build: `cc m2.c -o m2 -lvulkan`, run with `VK_DRIVER_FILES=…freedreno_icd… LD_LIBRARY_PATH=/opt/mesa-fd/lib/aarch64-linux-gnu`. |
| `glprobe.c` | gbm+EGL surfaceless GLES probe; reports `GL_RENDERER`. Proves zink→turnip GL on the EVDI gbm device. `cc glprobe.c -o glprobe -lgbm -lEGL -lGLESv2`; `MESA_LOADER_DRIVER_OVERRIDE=zink ./glprobe /dev/dri/card1`. |
| `zink-kgsl-pdev-fallback.patch` | 1-line Mesa patch: let zink pick the first Vulkan device when no DRM node matches (turnip on KGSL has none). Without it: `ZINK: failed to choose pdev`. |
| `bridge.cpp` | Standalone NDK binary (Android side, bionic, **no libhybris**): allocates a gralloc `AHardwareBuffer`, wraps it as the composer AIDL `HardwareBuffer`, calls `vendor.lindroid.composer` `IComposer.setBuffer`. `--selftest <displayId> [w h]` CPU-fills + presents. Replaces the crashing libhybris `create-disp`. |
| `aidl/…` | Hand-written wire-compatible subset of `android.hardware.graphics.common` (HardwareBuffer/HardwareBufferDescription/NativeHandle) so the composer AIDL can be codegen'd standalone with the NDK `aidl` tool. Enums parcel as their backing int/long. |
| `inc/android/binder_{manager,process}.h` | Shims — these libbinder_ndk headers ship in the platform, **not** the public NDK, so they're declared here and linked against the device's `libbinder_ndk.so`. |
| `proxy.py` | Tiny PC-side forward proxy for container networking over `adb reverse` (no Android `ip rule` changes). |

## Build the bridge (NDK)
```sh
NDK=$SDK/ndk/27.2.12479018/toolchains/llvm/prebuilt/<host>
# 1) generate composer AIDL NDK stubs (composer .aidl come from ../interfaces/composer in the Lindroid tree)
$SDK/build-tools/34.0.0/aidl --lang=ndk --structured -I aidl -o gen/src -h gen/include \
  aidl/**/*.aidl vendor/lindroid/composer/*.aidl
# 2) pull the device libs to link against (they export AServiceManager_*, AHardwareBuffer_getNativeHandle)
adb pull /system/lib64/libbinder_ndk.so devlibs/ ; adb pull /system/lib64/libnativewindow.so devlibs/ ; adb pull /system/lib64/liblog.so devlibs/
# 3) compile
$NDK/bin/clang++ --target=aarch64-linux-android34 --sysroot=$NDK/sysroot -std=c++17 -O2 -fPIE -pie \
  -static-libstdc++ -I inc -I gen/include bridge.cpp gen/src/**/*.cpp \
  -L devlibs -lbinder_ndk -lnativewindow -llog -o present-bridge
```

## Status
See [`../docs/native-turnip.md`](../docs/native-turnip.md) §6. GPU render + native present are
proven; a fully **visible** desktop is blocked by Motorola Ready For owning the panel and by
brand-new Adreno-8xx rough edges in the Mesa compositor GL path (KWin FBO-incomplete, sway crash).

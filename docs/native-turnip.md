# Native GPU (Turnip/freedreno on KGSL) — replacing libhybris entirely

Goal: GPU-accelerated Linux in the container **without libhybris**. The
[`native-display.md`](native-display.md) path renders through the Android GPU blob via
**libhybris**, which is stuck on an unsolved Android-16 bionic-TLS/gralloc port
(`create-disp` SIGSEGVs in the Android gralloc HAL; kwin SIGSEGVs in hybris EGL/GBM).

This document is the **other** approach: render with **upstream Mesa — Turnip (Vulkan) /
freedreno (GL)** talking **directly to the stock KGSL device** `/dev/kgsl-3d0`. No libhybris
in the render path, no Android EGL blob, decoupled from the Android userspace version.

**TL;DR of results**

| Piece | State |
|---|---|
| Turnip (Vulkan) enumerates the real GPU over stock KGSL, in-container | ✅ **proven** |
| Turnip renders + exports a dma-buf (correct readback) | ✅ **proven** |
| **OpenGL ES 3.2 via zink→turnip** on the EVDI gbm device (the KWin path) | ✅ **proven** (needs a 1-line zink patch) |
| Native, **libhybris-free** present bridge → SurfaceFlinger | ✅ **proven at the compositor level** (real SF layer, frames flowing) |
| A full **visible** GPU desktop (Plasma/sway) | ❌ blocked — see *Blockers* (Moto Ready For + a8xx compositor driver bug) |

The **hard unknowns are answered yes**: the GPU is reachable natively, render works, buffers
can be shared, and Android accepts our frames. What remains is (1) Motorola Ready For owning
the external panel and (2) brand-new Adreno-8xx rough edges in the Mesa compositor GL path.

Source for everything below is in [`../native-turnip/`](../native-turnip).

---

## 0. The hardware

- SoC **SM8845** (`canoe`), GPU **`Adreno 829`** — an **a8xx** part (Snapdragon-8-Elite-family),
  **not** Adreno 840. `cat /sys/class/kgsl/kgsl-3d0/gpu_model` → `Adreno829`.
- Vendor GPU driver package: `com.qualcomm.qti.gpudrivers.canoe.api36`.
- **`/dev/kgsl-3d0`** is char major **467** on this device (world-`rw`); the container opens it
  RDWR fine even though the Lindroid LXC config only cgroup-allows the stale major `237`
  (cgroup device controller isn't effectively enforcing here).
- There is **no drm/msm render node** — `# CONFIG_DRM_MSM is not set`. The DPU is exposed as
  `card0` (KMS, display-only, owned by SurfaceFlinger); EVDI is `card1`; render nodes
  `renderD128/129` belong to those, **not** the Adreno. So the GPU is reachable **only** via KGSL.
- Mesa main (26.2.0-devel) **already has this exact chip**:
  `freedreno_devices.py: GPUId(chip_id=0x44030a20, name="Adreno (TM) 829") # KGSL`, wired to
  `a8xx_base + a8xx_gen2`. **No gpu-db patch needed.**

---

## 1. Build Mesa for the container (Turnip + freedreno + KGSL)

All inside the Debian 13 (trixie) container. The distro Mesa (25.0.7) is both too old for a8xx
**and** built without the KGSL backend, so a source build is mandatory.

```sh
# deps
apt-get install -y build-essential meson ninja-build glslang-tools libglvnd-dev \
  libdrm-dev libwayland-dev wayland-protocols libwayland-egl-backend-dev \
  libexpat1-dev libzstd-dev zlib1g-dev libelf-dev vulkan-tools libvulkan-dev \
  mesa-utils cmake git bison flex python3-mako python3-yaml python3-ply

git clone --depth 1 https://gitlab.freedesktop.org/mesa/mesa.git
cd mesa
# apply native-turnip/zink-kgsl-pdev-fallback.patch  (needed for GL-on-KGSL, see §4)
meson setup build-fd -Dprefix=/opt/mesa-fd \
  -Dgallium-drivers=freedreno,zink -Dvulkan-drivers=freedreno \
  -Dfreedreno-kmds=kgsl \            # <-- THE key option: talk to /dev/kgsl-3d0 directly
  -Dplatforms=wayland -Dgbm=enabled -Degl=enabled -Dgles2=enabled \
  -Dglx=disabled -Dllvm=disabled -Dvulkan-layers= -Dbuildtype=release
ninja -C build-fd && ninja -C build-fd install
```

### External memory (dma-buf) prerequisite

Turnip needs a dma-heap to export/import dma-bufs. This device uses **dma_heap** (not ion) and
the container had neither. The host has `/dev/dma_heap/system` (major **249**, minor 1). Make it
persistent by adding to the container LXC config:

```
lxc.mount.entry = /dev/dma_heap dev/dma_heap none bind,optional,create=dir
```

(Live/one-shot: `mkdir -p /dev/dma_heap && mknod /dev/dma_heap/system c 249 1 && mknod /dev/dma_heap/qcom,system c 249 0 && chmod 666 /dev/dma_heap/*`.)
After that turnip advertises `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`,
`VK_KHR_external_memory_fd`.

### Container networking (needed to apt/clone)

The container shares Android's netns (`lxc.net.0.type = none`); Android policy-routes by fwmark,
so the container's **unmarked sockets have no route** ("No route to host"). **Do not** add host
`ip rule`s — that disturbs `netd` and destabilises the framework. Instead tunnel over USB:

```sh
adb reverse tcp:8899 tcp:8899          # device:8899 -> PC:8899
python proxy.py                         # tiny forward proxy on the PC (also does DNS)
# in the container:  http_proxy=http://127.0.0.1:8899  https_proxy=http://127.0.0.1:8899
```

(`proxy.py` is in [`../native-turnip/`](../native-turnip). Works over wireless adb too.)

---

## 2. M1 — Turnip enumerates the GPU over KGSL (the go/no-go)

```sh
export LD_LIBRARY_PATH=/opt/mesa-fd/lib/aarch64-linux-gnu
export VK_DRIVER_FILES=/opt/mesa-fd/share/vulkan/icd.d/freedreno_icd.aarch64.json
TU_DEBUG=startup vulkaninfo --summary
```
```
TU: info: Found compatible device '/dev/kgsl-3d0'.
deviceType   = PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
deviceName   = Adreno (TM) 829
driverID     = DRIVER_ID_MESA_TURNIP
apiVersion   = 1.4.354
```
**Native turnip drives the Adreno over stock KGSL, zero libhybris.** This is the whole thesis
in one command.

---

## 3. M2 — Render into a dma-buf, verified

[`../native-turnip/m2.c`](../native-turnip/m2.c): create a **linear DRM-modifier** `R8G8B8A8`
image on **exportable** memory, `vkCmdClearColorImage` to magenta, copy back, verify, export fd.

```
GPU: Adreno (TM) 829
R8G8B8A8_UNORM modifiers: LINEAR(0x0) + UBWC(0x0500000000000001)
matching pixels: 65536 / 65536
dma-buf export OK: fd=7, 262144 bytes
RESULT: PASS
```
Proves GPU execution + fence + dma-buf export. The format exposes both **LINEAR** (used here)
and Qualcomm **UBWC** (for zero-copy later).

---

## 4. OpenGL via zink→turnip (the KWin path)

freedreno's *gallium* GL driver can't bind through EGL/gbm here (no Adreno DRM node), so GL must
go through **zink** (GL-on-Vulkan) → turnip. Out of the box zink fails:

```
MESA: error: ZINK: failed to choose pdev
```
because `zink_get_display_device()` matches the Vulkan device to the display's DRM render node,
and turnip-on-KGSL has none. **One-line fix** (fall back to the first device — correct because
the render GPU is legitimately a different device from the EVDI scanout):
[`../native-turnip/zink-kgsl-pdev-fallback.patch`](../native-turnip/zink-kgsl-pdev-fallback.patch).

After that, GLES 3.2 renders on the GPU, including on the **EVDI gbm device** (`card1`) — the
exact path KWin uses ([`../native-turnip/glprobe.c`](../native-turnip/glprobe.c)):

```
device: /dev/dri/card1  gbm backend: drm
GL_RENDERER = zink Vulkan 1.4(Adreno (TM) 829 (MESA_TURNIP))
GL_VERSION  = OpenGL ES 3.2 Mesa 26.2.0-devel
RESULT: GPU
```

### Session env (replaces the libhybris block in `kwin-lindroid-hacks.sh`)
```sh
export LD_LIBRARY_PATH=/opt/mesa-fd/lib/aarch64-linux-gnu
export __EGL_VENDOR_LIBRARY_FILENAMES=/opt/mesa-fd/share/glvnd/egl_vendor.d/50_mesa.json
export VK_DRIVER_FILES=/opt/mesa-fd/share/vulkan/icd.d/freedreno_icd.aarch64.json
export MESA_LOADER_DRIVER_OVERRIDE=zink
# drop ALL of: GBM_BACKEND=hybris, __GLX_VENDOR_LIBRARY_NAME=libhybris,
#              EGL_PLATFORM=lindroid-drm, HYBRIS_PATCH_TLS, the 10_libhybris.json vendor
```

---

## 5. The libhybris-free present bridge

`ComposerImpl` (Android side, in `org.lindroid.ui`) is **already** libhybris-free — its
`setBuffer()` just takes any gralloc `AHardwareBuffer` + acquire fence and posts it via
`ASurfaceControl`/`ASurfaceTransaction` to SurfaceFlinger. The **only** libhybris user in the
present path is the container-side **`create-disp`**, which allocates buffers via
`hybris_gralloc_allocate` — and that **SIGSEGVs** in the Android gralloc HAL on A16 (same
breakage as `native-display.md`).

**Fix: flip where the buffer is allocated.** Allocate the gralloc buffer on the *Android* side
(native `AHardwareBuffer_allocate`, no libhybris), share the dma-buf to the container where
turnip renders into it, and post via the existing composer:

```
ANDROID side (native NDK, no libhybris)        CONTAINER side (turnip, no libhybris)
────────────────────────────────────          ─────────────────────────────────────
AHardwareBuffer_allocate  (real gralloc)
  → export dma-buf fd ───── socket/SCM_RIGHTS ───►  import into turnip (M2 proved this)
                                                     render the frame
  IComposer.setBuffer(hb, fence) ◄── frame ready ──  signal present
  → SurfaceFlinger → display
```

[`../native-turnip/bridge.cpp`](../native-turnip/bridge.cpp) is the standalone NDK binary that
does the Android side. It gets `vendor.lindroid.composer` via `libbinder_ndk`, allocates an
`AHardwareBuffer`, wraps its gralloc handle as the AIDL `HardwareBuffer`, and calls `setBuffer`.
It's built with the NDK; `libbinder_ndk`/`libnativewindow` are pulled from the device and the two
system-only headers (`binder_manager.h`, `binder_process.h`) are shimmed
([`../native-turnip/inc`](../native-turnip/inc)). The AIDL imports
(`android.hardware.graphics.common.HardwareBuffer` etc.) are hand-written wire-compatible
subsets ([`../native-turnip/aidl`](../native-turnip/aidl)) — enums parcel as their backing
int/long, so `HardwareBufferDescription` uses plain `int format; long usage;`.

**Result — proven at the compositor level:** the bridge's `--selftest` allocates a buffer,
CPU-fills it, and calls `setBuffer` per frame:
```
got composer; display=4
allocated AHB stride=2496 format=1 usage=819   ← native gralloc alloc, NO libhybris
frame 0..N setBuffer ret=0                       ← composer imports (createFromHandle OK) + posts
```
SurfaceFlinger shows a real live layer `LindroidDisplay#640` with our buffer, frames incrementing,
on the external display's layer stack. **Android accepts natively-allocated frames from a
libhybris-free source.** (The container-render + socket half is the remaining integration; every
piece it needs — turnip render, dma-buf import/export — is already proven.)

Build:
```sh
NDK=.../ndk/27.2.12479018/toolchains/llvm/prebuilt/<host>
$NDK/bin/clang++ --target=aarch64-linux-android34 --sysroot=$NDK/sysroot \
  -std=c++17 -O2 -fPIE -pie -static-libstdc++ -I inc -I <aidl-ndk-gen-include> \
  bridge.cpp <generated-aidl-stubs>.cpp -L devlibs -lbinder_ndk -lnativewindow -llog \
  -o present-bridge
# generate stubs:  <build-tools>/aidl --lang=ndk --structured -I aidl -o gen/src -h gen/include <all .aidl>
```

---

## 6. Blockers (why there's no full on-screen desktop yet)

1. **Motorola Ready For owns the external panel.** Our frames land on a real SF layer on the
   external display's layer stack (12), but the physical panel (`CX158`, uniqueId
   `4613397687650017301`) is a **"Follower"/"inactive"** HWC display and Ready For's
   `SecondaryDisplayLauncher` composites its own desktop on it; our logical display isn't scanned
   out. `ro.product.motodesktop=1` bakes Ready For in; force-stopping it just respawns. The clean
   fix is to draw **directly** onto the physical display via `libgui` `SurfaceControl` at top
   z-order (needs AOSP headers / C++ ABI — not attempted) **or** wipe/reconfigure so the external
   display isn't Ready-For-managed.

2. **Adreno-8xx compositor GL path is immature in Mesa main.** turnip/zink is flawless for
   **offscreen/surfaceless** GPU work (M1, M2, glprobe all pass), but full desktop compositors
   fail:
   - **KWin** virtual backend → `kwin_scene_opengl: GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT`
     (both `KWIN_COMPOSE=O2` and `O2ES`). Plasma *processes* launch on turnip but nothing composites.
   - **sway** (`WLR_BACKENDS=headless`, `WLR_RENDERER=gles2`, zink) crashes ~0.4 s in (wlroots
     broken pipe).
   - turnip spams `vkGetCalibratedTimestampsEXT VK_ERROR_OUT_OF_HOST_MEMORY` (likely an a8xx
     turnip bug).
   These are **Mesa-level a8xx rough edges** (a8xx landed in Mesa main very recently). Next steps:
   debug the zink→turnip FBO-completeness/format path, try `TU_DEBUG`/zink workarounds, or test the
   KWin **DRM backend on EVDI** (vs virtual) to see if it dodges the FBO bug.

---

## 7. Field notes / gotchas (cost real time — save yourself)

- **Folded-closed + a live composer wedges SurfaceFlinger → reboot.** The LindroidUI composer's
  `VsyncThread` (DisplayEventReceiver hammering SF) while the phone is folded shut caused repeated
  `system_server` watchdog kills / full reboots. Keep the phone **unfolded** while any composer/
  DisplayActivity is up. (Also: the container's `/apex` bind-mounts block `apexd` on a framework
  restart, turning a wedge into a black-screen bootloop — masked/`systemctl mask create-disp` and
  keep the container headless for non-display work.)
- **`WiredAccessoryManager` NPE** on a wired-accessory (USB-C/DP) uevent can kill `system_server`
  (also patched in the stock services.jar work).
- **Wireless adb, persistent** (needed once the USB-C port is taken by the external display):
  `setprop persist.adb.tcp.port 5555` + a `/data/adb/service.d/wireless_adb.sh` that re-applies
  it and restarts `adbd` each boot.
- **`am start --display N`** into a Ready For display puts the app in a *freeform desktop window*,
  not fullscreen; and `ComposerImpl::setBuffer` returns `NO_ERROR` even when the display has no
  live surface — so "ret=0" does **not** mean it's on screen. Check the actual SF layer / screencap.

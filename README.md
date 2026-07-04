# Lindroid on the Motorola Razr Fold 2026 — **stock Android 16, no custom ROM**

Running **Lindroid** (a full GNU/Linux distro in an LXC container with a hardware path to
the phone's display) on a **bone-stock Motorola Razr Fold 2026** (`blanc`, Snapdragon 8
Gen 5 / SM8845, **Android 16**, GKI `6.12.38-android16-5`) — rooted with Magisk, **without
flashing a custom ROM**.

Lindroid officially targets custom ROMs (LineageOS / LMODroid) because it needs an AOSP
patchset. This repo does it on the **stock** ROM by shipping the Android-side pieces as
Magisk modules, rebuilding the native libraries against Android 16, and patching the stock
`services.jar` and the kernel where stock behaviour blocks it.

|  |  |
|---|---|
| **Device** | Motorola Razr Fold 2026 · `blanc` (`blanc_g`) · Verizon |
| **SoC / GPU** | Snapdragon 8 Gen 5 = SM8845 · Adreno · DPU "Eliza" |
| **OS / kernel** | Android 16 · GKI `6.12.38-android16-5-…-4k` · build `W3WBS36.36-48-5-1` |
| **Root** | Bootloader unlocked + Magisk (init_boot) |
| **Companion repo** | [`razr-fold-2026-kernel-build`](https://github.com/zorrobyte/razr-fold-2026-kernel-build) — the from-source kernel (containers + EVDI + the fix below) |

---

## Status

| Piece | State |
|---|---|
| Kernel: LXC container configs + EVDI/lindroid-drm driver | ✅ working (from-source kernel) |
| **Kernel: EVDI `open()` on 6.12** (`FOP_UNSIGNED_OFFSET`) | ✅ **fixed** (see below) — cards open, no runaway |
| `perspectived` + `liblxc` + `lxc_*` tools | ✅ built from source (NDK), running |
| `services.jar`: let `org.lindroid.ui` join `android.uid.system` | ✅ patched → app runs as system, boots clean |
| `services.jar`: `WiredAccessoryManager` NPE from EVDI uevent | ✅ patched → no reboot |
| A16 native libs (`libjni_lindroidui`, composer, `libhwc2/ui_compat_layer`) | ✅ built via AOSP/Soong |
| App installs, launches, **creates + starts the Debian container** | ✅ working |
| **Linux desktop over VNC (software-rendered)** | ✅ **working** — usable today |
| **Native hardware-accel desktop on the phone screen** | ⏳ blocked on a libhybris/A16 gralloc fix (diagnosed; patch in progress) |

**Bottom line:** the whole stack works end-to-end except the final *hardware-accelerated*
on-screen path. You get a **usable KDE/XFCE Linux desktop today over VNC**, and the native
path is down to a single, precisely-diagnosed libhybris incompatibility with Android 16.

---

## Architecture

Lindroid has two sides:

* **Android side** (shipped here as Magisk modules): the kernel (containers + **EVDI**
  virtual display), `perspectived` (the LXC-managing binder daemon), the `lxc_*` tools,
  `LindroidUI` (the app + its HWComposer-emulation HAL `libjni_lindroidui` +
  `vendor.lindroid.composer`), and the `libhwc2_compat_layer`/`libui_compat_layer` shims.
* **Container side** (prebuilt in the Lindroid rootfs): Debian + KDE Plasma + `kwin_wayland`
  + **libhybris** (loads the phone's Adreno GPU + gralloc blobs into the glibc container),
  `create-disp` (creates the EVDI display), SDDM, PipeWire, etc.

The two share graphics buffers through the **EVDI** virtual DRM device: `kwin` renders into
EVDI, the Android composer reads the frames and paints them on the phone screen.

---

## The fixes (what stock does that blocks Lindroid, and how it's beaten)

### 1. Kernel — EVDI card `open()` returns `EINVAL` on 6.12
On kernel 6.12, `drm_open_helper()` (`drivers/gpu/drm/drm_file.c:316`) requires the DRM
driver's `file_operations` to set **`FOP_UNSIGNED_OFFSET`**; without it, **every `open()` of
an EVDI `/dev/dri/cardN` fails with `EINVAL`**. The vendored EVDI driver uses a custom
`evdi_fops` (not `DEFINE_DRM_GEM_FOPS`, which sets the flag on 6.12), so it was missing it.
This single failure caused the black screen: container `create-disp` could never find a card
and spun writing `/sys/devices/evdi-lindroid/add` (leaking 40+ cards), and `kwin_wayland`
reported `failed to open drm device`.

**Fix:** add `.fop_flags = FOP_UNSIGNED_OFFSET` (under `#ifdef`) to `evdi_fops`. Committed in
the [kernel repo](https://github.com/zorrobyte/razr-fold-2026-kernel-build) (`9bb148a`).
Verified: fresh boot = 1 card, `open(card1, O_RDWR)` now succeeds. See
[`patches/kernel/`](patches/kernel/).

### 2. `services.jar` — signature wall for `android.uid.system`
`LindroidUI` declares `sharedUserId="android.uid.system"` and is signed with the AOSP
**test-key**. On stock, the `android` package is signed with Motorola's private platform
key, so PackageManager throws `Signature mismatch on system package org.lindroid.ui` and
`system_server` bootloops. **Fix:** patch `PackageManagerServiceUtils.canJoinSharedUserId()`
in `services.jar` to allow the join (dex-patched, whiteout the stale oat so ART loads the
patched dex). See [`patches/services-jar/`](patches/services-jar/) and
[`docs/services-jar.md`](docs/services-jar.md).

### 3. `services.jar` — `WiredAccessoryManager` NPE from the EVDI uevent
When the EVDI display hotplugs, Moto's customized `WiredAccessoryManager.updateLocked()`
does `startsWith()` on a null name → NPE → `system_server` dies → RescueParty reboot.
**Fix:** null-guard the top of `updateLocked()`. Same dex-patch pipeline.

### 4. A16 native libraries (the A14 prebuilts crash on A16)
The community prebuilt `libjni_lindroidui.so` segfaulted on A16's `Surface`/`SurfaceControl`
ABI. Rebuilt `libjni_lindroidui`, `vendor.lindroid.composer-ndk`, `libhwc2_compat_layer` and
`libui_compat_layer` from `vendor_lindroid`/`libhybris` source **against Android 16 via
AOSP/Soong**. See [`docs/native-libs.md`](docs/native-libs.md).

### 5. VNC desktop (works today)
Software-rendered XFCE over `Xvfb` + `x11vnc`, viewed via `adb forward tcp:5901`. Key trick:
the rootfs globally forces the libhybris GPU stack, whose A14-era HIDL HALs crash Xorg on
A16, so the VNC session forces **pure-software mesa** (`__EGL_VENDOR_LIBRARY_FILENAMES`
→ `50_mesa.json`, `LIBGL_ALWAYS_SOFTWARE=1`) and uses XFCE (the rootfs Qt6 is Wayland-only,
no xcb). See [`scripts/vnc-desktop.sh`](scripts/vnc-desktop.sh) and
[`docs/vnc.md`](docs/vnc.md).

### 6. Native display — the remaining blocker (diagnosed)
With the kernel fix, `create-disp` **finds and opens** the EVDI card, then segfaults inside
**libhybris** while allocating the display gralloc buffer (`hybris_gralloc_allocate`).
Root cause: A16's bionic gralloc libs are stack-protected, and Lindroid's libhybris (a
**2020-era base**) **doesn't hook `__stack_chk_fail`/`__*_chk`** and doesn't keep bionic's
TLS stack-guard slot intact — so a canary check fires and jumps to an unresolved stub.
The one-symbol proof: libhybris logs *"Could not find a hook for symbol `__stack_chk_fail`"*.
The fix (add the missing hooks to `hybris/common/hooks.c` and rebuild libhybris-common) is in
[`patches/libhybris/hooks-stack_chk-A16.patch`](patches/libhybris/hooks-stack_chk-A16.patch);
completing it needs the `android-headers` build package (Lindroid's flaky repo). This is
effectively a **Lindroid-upstream Android-16 port** of libhybris. See
[`docs/native-display.md`](docs/native-display.md).

---

## Repo layout

```
modules/        Magisk modules (lindroid_stock, services_lindroid) — trees minus big binaries
patches/        kernel EVDI fix, services.jar smali patches, libhybris hooks patch
native-libs/    notes on the AOSP/Soong build of the A16 native libs
scripts/        build + setup scripts (NDK userspace, VNC desktop, libtls-padding, EVDI test)
docs/           deep-dives per area
```

Big binaries (the LindroidUI apk, the built `.so`s, the 1.8 GB rootfs, `services.jar`) are
**not** committed — see each `docs/` page for where to get / how to build them.

---

*Built by zorrobyte with Claude. Not affiliated with the Lindroid project.*

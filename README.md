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
| libhybris: missing bionic `__stack_chk_fail`/`__*_chk` hooks (A16) | ✅ **fixed** — rebuilt `libhybris-common` in-container; create-disp now reaches EGL init |
| **Native hardware-accel desktop on the phone screen** | ⏳ root-caused to the libhybris **TLS patcher** vs. A16 bionic (a libhybris port) |

**Bottom line:** the whole stack works end-to-end except the final *hardware-accelerated*
on-screen path. You get a **usable KDE/XFCE Linux desktop today over VNC**. The native path is
now root-caused to one specific libhybris-internals issue: its 2020-era **TLS patcher** doesn't
set up the TLS shadow table for Android 16's bionic, so `create-disp` null-derefs a patched
TLS access during EGL init. Fixing it is an Android-16 port of libhybris — see
[`docs/native-display.md`](docs/native-display.md).

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

### 6. libhybris — missing bionic fortify/stack-protector hooks (A16) — FIXED
With the kernel fix, `create-disp` **finds and opens** the EVDI card, then segfaulted inside
**libhybris**: A16's bionic graphics libs are stack-protected + `_FORTIFY_SOURCE`, and
Lindroid's 2020-era libhybris didn't hook `__stack_chk_fail`/`__*_chk`, so those imports
resolved to unhooked stubs → jump to garbage. Proof: *"Could not find a hook for symbol
`__stack_chk_fail`"*. **Fix:** add the hooks and rebuild `libhybris-common` **in-container**
([`patches/libhybris/hooks-stack_chk-A16.patch`](patches/libhybris/hooks-stack_chk-A16.patch),
[`scripts/build-libhybris-common-A16.sh`](scripts/build-libhybris-common-A16.sh)). glibc lacks
`__write_chk`, so the `_chk`s are local wrappers, not `extern`s. The `android-headers`
build-dep comes from Halium's repo with `android-version.h` bumped to Android 11 (Lindroid's
own repo truncates the deb). Verified: crash gone, create-disp reaches EGL init.

### 7. Native display — the remaining blocker (root-caused)
Past the hooks fix, `create-disp` crashes in **EGL init** at `ldr x8,[x8,#0x820]` with a NULL
`x8`. That instruction isn't in the on-disk bionic libc — **libhybris rewrote it in memory**:
its **TLS patcher** turns bionic `mrs tpidr_el0` reads into loads from its own TLS shadow
table, and the table base is null on create-disp's thread. Root cause: the 2020-era libhybris
TLS patcher doesn't set up the shadow table for **Android 16's bionic TLS layout**. `HYBRIS_PATCH_TLS`
and the `libtls-padding` preload don't help — the *table setup* is what's wrong. The device
does have HIDL/QTI gralloc + binder available (not the blocker). Fixing this is an Android-16
port of libhybris's TLS patcher. Full evidence in [`docs/native-display.md`](docs/native-display.md).

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

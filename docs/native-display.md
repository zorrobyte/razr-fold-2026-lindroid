# Native (hardware-accelerated) desktop — full diagnosis

Goal: the container's KDE Plasma / kwin desktop rendered **on the phone's own screen**,
GPU-accelerated, via `kwin_wayland` → EVDI → the Android composer. This is the "real"
Lindroid convergence path (vs. the software VNC path in [`vnc.md`](vnc.md)).

The pipeline has **four** layered blockers on stock Android 16. The first three are fixed;
the fourth is diagnosed to root cause and is the remaining work.

`create-disp` is the container-side tool that creates the shared EVDI display buffer and is
the first thing that runs. Each layer below is "how far create-disp gets."

## Layer 1 — kernel: EVDI `open()` → `EINVAL` — **FIXED**

Symptom: black screen; `create-disp` restart-loops "evdi-lindroid still not available",
`kwin_wayland_drm: failed to open drm device`, and `/sys/class/drm` leaks dozens of `card*`.

Root cause: kernel 6.12 `drm_open_helper()` (`drm_file.c:316`) requires
`FOP_UNSIGNED_OFFSET` in the DRM driver's `file_operations`; the vendored EVDI driver's
custom `evdi_fops` didn't set it, so **every** EVDI `open()` returned `EINVAL`.

Fix — [`../patches/kernel/evdi-FOP_UNSIGNED_OFFSET.patch`](../patches/kernel/evdi-FOP_UNSIGNED_OFFSET.patch),
committed to the kernel repo (`9bb148a`). Verified: fresh boot = 1 card, `open(card1)` succeeds.

## Layer 2 — userspace: create-disp finds and opens the card — OK
With the kernel fix, `create-disp` (systemd svc, `User=lindroid`, `video`+`render` groups)
logs `Found evdi-lindroid at /dev/dri/card1` and opens it. Then it crashes.

## Layer 3 — libhybris: missing bionic fortify/stack-protector hooks — **FIXED**

`create-disp` SIGSEGV'd immediately after opening the card. With `HYBRIS_LOGGING_LEVEL=debug`
(requires libhybris built **without** `--disable-trace`), the cause was explicit:

```
__hybris_get_hooked_symbol: Could not find a hook for symbol __stack_chk_fail
```

Android 16's bionic gralloc/graphics libs are compiled stack-protected + `_FORTIFY_SOURCE`.
Lindroid's libhybris (a 2020-era base) doesn't hook `__stack_chk_fail` / `__*_chk`, so those
imports resolved to unhooked stubs → jump to garbage → SIGSEGV.

Fix — rebuild `libhybris-common` with the hooks added:
[`../patches/libhybris/hooks-stack_chk-A16.patch`](../patches/libhybris/hooks-stack_chk-A16.patch).
Note glibc has `__read_chk`/`__vsnprintf_chk` but **not** `__write_chk`, so all three are
local wrappers, not plain `extern`s (the naïve `extern __write_chk` gives an undefined-symbol
load error). Build recipe below. **Verified:** the "Could not find a hook" crash is gone;
`create-disp` now loads the Q linker, opens card1, and proceeds into EGL/graphics init.

### Building libhybris-common for A16 (in-container, aarch64-native)
`create-disp`'s libhybris runs in the container, which is aarch64 — so build it there, not in
an x86 cross env. Recipe: [`../scripts/build-libhybris-common-A16.sh`](../scripts/build-libhybris-common-A16.sh)
(+ [`../patches/libhybris/fix-chk-hooks.py`](../patches/libhybris/fix-chk-hooks.py)). Key points:

* **android-headers**: libhybris `configure` needs the `android-headers` pkg-config module.
  Lindroid's apt repo (`repo.lindroid.org`) serves a `android-headers-30` deb but the transfer
  truncates and never completes. Use **github.com/Halium/android-headers** instead (complete
  tree + `.pc`), but `sed` its `android-version.h` to `ANDROID_VERSION_MAJOR 11` — Halium's is
  `5.1.1`, and `configure` gates `-DWANT_LINKER_Q` (needed or `LINKER_NAME_DEFAULT` is
  undeclared → compile error) on `HAS_ANDROID_10_0_0`, which it derives from that header. API
  30 = Android 11 is what the real `android-headers-30` reported, so this reproduces the stock
  build exactly.
* `./configure` needs `--build=aarch64-linux-gnu --host=aarch64-linux-gnu` (its bundled
  `config.guess` is too old to detect aarch64).
* Build only `make -C common`; install over `/usr/lib/aarch64-linux-gnu/libhybris-common.so.1`
  (keep a `.orig`).

## Layer 4 — libhybris TLS patcher vs. Android-16 bionic — **the remaining blocker (root-caused)**

After Layer 3, `create-disp` gets into **EGL / graphics init** (last trace activity is in the
`EGL` subsystem touching `ANDROID_TOP_ADDR_VALUE_MUTEX`) and SIGSEGVs. gdb:

```
Program received signal SIGSEGV
=> 0x…ccc:  ldr  x8, [x8, #2080]      ; #2080 = 0x820
x8  = 0x0                              ; NULL base
x0/x19/x20 = 0xffffffffffffffff        ; error sentinels
```

The faulting PC lies inside **`/apex/com.android.runtime/lib64/bionic/libc.so`** (the Android
bionic libc that libhybris loads for the graphics libs). But disassembling that **file** offset
shows a completely different instruction (`b.cs`, inside bionic's regex `p_ere_exp`). The
in-**memory** instruction differs from the on-disk one because **libhybris rewrote it**: its
TLS patcher scans loaded bionic libs for thread-pointer reads (`mrs xN, tpidr_el0`) and
rewrites them to load from libhybris's own TLS shadow table. `ldr x8, [x8, #0x820]` is such a
rewritten access — and the shadow-table base (`x8`) is **NULL** on the thread `create-disp`
runs its graphics init on.

**Root cause:** the 2020-era libhybris TLS patcher does not correctly set up the TLS shadow
table for **Android 16's bionic TLS layout / slot count**. So a patched bionic TLS access reads
a null table base → null deref. (Same `0x820`-offset signature as the Xorg crash the VNC path
dodges by not using libhybris at all.)

Ruled out along the way:
* `HYBRIS_PATCH_TLS=1` — no change (patching already happens; the *table setup* is what's wrong).
* Preloading `libtls-padding.so` (the mer-hybris `tls_padding[16]` shim) — no change.
* "A16 is AIDL-only / has no HIDL gralloc" (an earlier theory) — **false**: the device ships
  `graphics.mapper@2.1`…`@4.0` and the QTI gralloc (`/vendor/lib64/hw/mapper.qti.so`,
  `libgralloc.qti.so`, `…mapper@4.0-impl-qti-display.so`), all visible in the container, and the
  container has `/dev/binder`,`/dev/hwbinder`,`/dev/vndbinder`. The blocker is TLS, not HAL
  availability.

### The exact fault
Disassembling **live memory** at the crash (gdb, so the bytes are what actually executes):

```
      bti  c                      ; a bionic libc function entry
      mrs  x8, tpidr_el0          ; x8 = the REAL thread pointer (NOT patched to a thunk)
      ldur x8, [x8, #-8]          ; x8 = *(TP-8) : a bionic per-thread TLS slot  ->  loads 0
  =>  ldr  x8, [x8, #2080]        ; *(NULL + 0x820)  ->  SIGSEGV   (x8 = 0x0)
      add  x9, x8, #1             ; (it was about to bump a per-thread counter)
```

So a bionic libc function reads a per-thread TLS slot at `[TP-8]` and gets **NULL**, because
`create-disp` runs this on its **glibc main thread**, whose `tpidr_el0` points at glibc's TCB
— there is no bionic TLS there. The `mrs` was **not** rewritten (no thunk), because Lindroid's
`create-disp.service` sets **no `HYBRIS_PATCH_TLS`** (the thunk patcher is off by default), and
even the patcher only handles TLS offsets ≤ `0xFFF` and bails otherwise.

### Is it fixed anywhere? — No (checked GitHub, July 2026)
* `Linux-on-droid/libhybris`: the thunk-based TLS patcher (`hybris/common/tls_patcher*.c`,
  commit `75be4aab`, Feb 2025) is the **newest** TLS code on **every** branch
  (`lindroid-drm`, `new-gbm-surface`, `tmp`, `vk-hacks`, …). Nothing supersedes it.
* libhybris issue **#559** ("Saving/restoring TLS pointer before/after entering bionic
  functions", Jul 2024) proposes the *correct* fix — allocate a separate bionic TLS per
  thread, swap `tpidr_el0` in before each wrapped bionic call and restore it after — but it was
  **never implemented**. The thunk patcher is a partial alternative and isn't wired into
  `create-disp`.

**Conclusion:** this is an **open, unsolved problem in libhybris upstream**, not a missing
config on our side. `create-disp` needs bionic TLS set up on the thread it runs graphics init
on. The real fixes are, in order of soundness:
1. Implement issue #559 (per-thread bionic TLS + `tpidr_el0` swap around bionic calls) in the
   hybris linker — the robust solution, substantial libhybris work.
2. Make the hybris linker allocate/init bionic static TLS for the calling (glibc) main thread
   so `[TP-8]` is valid without patching.
3. Whack-a-mole: `HOOK_TO` every bionic libc function the graphics path calls that touches
   bionic TLS, redirecting to glibc (same technique as the `__stack_chk_fail` fix in Layer 3).
   Tractable but open-ended — the TLS-access pattern recurs across many bionic functions.

## Update — the crashes ARE fixable (whack-a-mole hooks), and the desktop now RUNS

The "unimplemented upstream" conclusion above was too pessimistic. The bionic-TLS accesses
that crash `create-disp` are a **small, closed set**, not "all of bionic". Resolving each
crash PC to a libc symbol (`crashPC − libcbase + fileoff` → `nm -D -n` nearest symbol) and
hooking that symbol to glibc converged in a few iterations:

* **Locale family** — the first crash was `__ctype_get_mb_cur_max` (reads the thread-local
  locale via bionic TLS → NULL on a glibc thread). glibc exports the whole family, so
  `HOOK_TO` them: `__ctype_get_mb_cur_max`, `__ctype_b_loc`, `__ctype_tolower_loc`,
  `__ctype_toupper_loc`. → `create-disp` stops crashing at that point.
* **CFI** — next crash was `__cfi_slowpath` (bionic Control Flow Integrity) dereferencing the
  `__cfi_shadow` map, which the hybris linker never initializes → null deref. No-op
  `__cfi_slowpath`/`__cfi_slowpath_diag` (CFI is a security check that can't work under
  libhybris). → `create-disp` runs stably.

All in [`../patches/libhybris/hooks-stack_chk-A16.patch`](../patches/libhybris/hooks-stack_chk-A16.patch).

**kwin** crashed too (GLES render via libhybris/Adreno). Two fixes: set `KWIN_COMPOSE=Q`
(QPainter software compositing — skips the crashing Adreno GL path) in the SDDM greeter env,
and fix the greeter's `LD_PRELOAD=/usr/lib/libtls-padding.so` (the rootfs ships it only at
`/usr/lib/aarch64-linux-gnu/` — symlink it). With these, **the full KDE Plasma session runs**
(`startplasma-wayland` + `kwin_wayland` + `plasmashell` all alive, verified via `ps`).

## The genuine remaining blocker — EVDI display never connects

With the container desktop running, the last gap is **display lifecycle coordination**, and it
is NOT yet solved: the EVDI output never actually connects. `/sys/class/drm/card1-Virtual-*/status`
stays `disconnected` (truly 0 connected — beware `grep connected` also matches *dis*connected),
and the EVDI plane stays `crtc=(null) fb=0`, so **kwin renders headless and the phone shows
black.** `create-disp` finds the card (`Found evdi-lindroid at card1`) and no longer crashes,
but the composer↔create-disp↔kwin handshake that should (a) make the composer request a
display, (b) have `create-disp` connect an EVDI output at the phone resolution, and (c) have
kwin hotplug-detect and render to it — does not reliably complete. `create-disp`'s systemd
service is also unstable (intermittent restarts). This is the "black screen last mile": the
libhybris/graphics crashes are fixed and the desktop runs, but its frames don't reach the
screen yet.

### The true final blocker — the LindroidUI app wedges Android 16's WindowManager
Tracing the full handoff (create-disp source: `Linux-on-droid/create-disp`; composer:
`vendor_lindroid/app/app/src/main/cpp/ComposerImpl.cpp`):

1. DisplayActivity SurfaceView → JNI `nativeSurfaceChanged` → `ComposerImpl::onSurfaceChanged`
   registers a display **with a nativeWindow**.
2. create-disp connects via libhybris HWC2 → the composer's `registerCallback` hotplugs every
   display that has a nativeWindow (`onHotplugReceived`).
3. create-disp's `onHotplugReceived` → `evdi_connect(w,h,…)` → the EVDI connector goes
   *connected*, kwin renders, and create-disp's poll loop forwards frames back to the SurfaceView.

**It never gets to step 1.** Native `onSurfaceChanged` never fires — verified: the composer
logs `Starting composer binder service` and create-disp's `registerCallback: sequenceId: 0`,
but there is never an `onSurfaceChanged: Display:` line. The reason is upstream of everything:
the **LindroidUI app windows never finish drawing** — `BLASTSyncEngine: Sync group NN timeout`,
`Unfinished container: …DisplayActivity/LauncherActivity`, `mCurrentFocus=null`. The
SurfaceView surface never finalizes → no nativeWindow → nothing to hotplug → create-disp waits
forever → kwin headless → black.

**And it's worse than black:** the stuck BLAST sync escalates to a **WindowManager watchdog
kill of `system_server`** (verified: `system_server` pid disappears, `dmesg` shows the
watchdog, a cascade of app `FATAL EXCEPTION`s). So opening the Lindroid display on this stock
Android 16 build **crash-reboots the phone's UI**. The composer's `nativeStartComposerService`
ends in `ABinderProcess_joinThreadPool()` (blocks forever — fine only if Java calls it off the
main thread), and the device runs a **newer LindroidUI apk** (`LauncherActivity`+`DisplayActivity`)
than the public repo HEAD (`MainActivity`), so the exact Java threading around the SurfaceView
couldn't be audited. The root is an **Android-16 WindowManager/BLAST incompatibility in the
LindroidUI app's display path** — a distinct problem from the (now-fixed) libhybris graphics
crashes, and one that requires app-side (Java + native) work, not container-side.

## Update 2 — the BLAST wedge is the FOLD STATE, and the display pipeline works

Two big findings on a test device:

**1. The DisplayActivity BLAST wedge only happens folded-closed.** With the phone **unfolded**
(main inner display active, `device_state=OPENED`), the LindroidUI windows draw normally
(`mCurrentFocus=…DisplayActivity`, no `Sync group timeout`), and native
`ComposerImpl::onSurfaceChanged` **fires** (`Display: 0, 2232x2368`). Folded-closed (cover
display) it wedges → watchdog → `system_server` crash-loop (needs a sysrq reboot to recover).
So: **keep it unfolded.** This was the real cause of the "app crashes the phone" behaviour.

**2. The full display-connection pipeline WORKS.** Unfolded, the entire chain completes:
`onSurfaceChanged` → composer registers a display with a `nativeWindow` → create-disp
`registerCallback` → composer `onHotplugReceived` → create-disp `evdi_connect(2232,2368)` →
**`/sys/class/drm/card1-Virtual-3/status = connected`** (verified with `[ "$s" = connected ]`,
not the substring-buggy `grep`). The EVDI output connects at the inner-display resolution. This
is the "black screen last mile" (task #8) coordination — **solved up to the connection.**
Timing matters: create-disp must `registerCallback` while the DisplayActivity surface is live.

## The genuine final blocker — kwin crashes rendering to the EVDI output

With Virtual-3 connected, kwin still won't drive it: `enabled=disabled`, `evdi plane fb=0`,
`crtc=(null)`, and kwin is **not DRM master** (`master=n`) even though card1 is correctly
seat0-tagged (`master-of-seat`) and `udevd` is running. The kwin journal shows it initializes
fine — `Compositing forced to QPainter mode`, `QPainter compositing successfully initialized`,
effects load — then **`KCrash: kwin_wayland crashing`**. So kwin **crashes** during DRM-output
enablement / first present. Crucially, even in QPainter (software) mode kwin logs
`Forcing EGL native interface as Qt uses OpenGL ES` — it still initializes the **hybris EGL/GBM
on the EVDI render node** (`renderD129`, `GBM_BACKEND=hybris`), and that path — which
`create-disp` does *not* exercise (create-disp uses the HWC2 compat path) — is almost certainly
where it dies, on a libhybris function not covered by the Layer-3 hooks.

**strace narrows it to the hybris EGL/GBM init.** Wrapping `kwin_wayland` in `strace`
(which, unlike gdb, preserves the `--wayland-fd` passing) shows that immediately before the
crash kwin loads `libgbm.so.1`, hybris `libGLESv2.so.2` (RPATH gives it away:
`/tmp/tmp.*/source/obj-aarch64-linux-gnu/.../libGLESv2.so.2` — the libhybris build tree) and
`libEGL.so.1`, and then dies — with **no `/dev/dri` / `renderD129` ioctls yet**. So kwin
SIGSEGVs inside the **hybris EGL/GBM setup** (`GBM_BACKEND=hybris` → `gbm_create_device` /
`eglGetPlatformDisplay(GBM)` / GLES init) *before* it ever does a DRM commit. This is exactly
the code path `create-disp` does **not** take (create-disp reaches the display via the HWC2
compat layer, not GBM), which is why create-disp survives and kwin doesn't.

**Could not get the exact faulting symbol**: this container can't write core dumps (Android
kernel `core_pattern`), gdb-wrapping breaks `--wayland-fd`, `KCrash` double-faults
(`crashRecursionCounter=2`) before dumping frames, `wayland-info` hangs, `libSegFault.so` isn't
present, and `strace -k` had no unwinder. The next step is to instrument **libgbm-hybris / the
hybris EGL GBM platform** (or run on a device where core dumps work) to catch the SIGSEGV → the
missing libhybris function then becomes another `HOOK_TO`, exactly like the locale/CFI fixes.

## Update 3 — EXACT crash caught via a minimal reproducer (cores DO work)

Core dumps actually work in the container (`CONFIG_COREDUMP=y`; a test crasher dumped fine).
kwin didn't dump only because KCrash double-faults and its `RLIMIT_CORE` was 0. Rather than fight
KCrash, a **~40-line reproducer** doing exactly what kwin's DRM backend does (`gbm_create_device`
→ `eglGetPlatformDisplay` → `eglInitialize` → `eglChooseConfig` → `eglCreateContext` →
`gbm_surface_create` → `eglCreateWindowSurface`), run under gdb, gives the exact backtrace:

```
#0 pthread_mutex_lock                                   libc.so.6   (bad mutex ptr)
#1 wl_proxy_create_wrapper                              libwayland-client.so.0
#2 WaylandNativeWindow::WaylandNativeWindow(wl_egl_window*, wl_display*, android_wlegl*)
                                                        eglplatform_lindroid-drm.so
#3 lindroid_drmws_CreateWindow                          eglplatform_lindroid-drm.so
#4 eglCreateWindowSurface                               libEGL_libhybris.so.0
```

Everything through `gbm_surface_create` succeeds (`eglInitialize=1 ver=1.5`); the crash is in
**`eglCreateWindowSurface`**.

**Root cause** (platform source `hybris/egl/platforms/lindroid_drm/eglplatform_lindroid_drm.cpp`):
the `lindroid_drm` EGL platform is a **wayland** platform. `lindroid_drmws_GetDisplay` does
`wdpy->wl_dpy = (wl_display*)nativeDisplay; if (!wl_dpy) wl_dpy = wl_display_connect(NULL);` and
`lindroid_drmws_CreateWindow` builds a `WaylandNativeWindow(wl_egl_window*, wl_dpy, android_wlegl*)`
— it bridges container GL buffers to the Android composer over the **`android_wlegl`** wayland
protocol. But kwin's **DRM backend passes a GBM device** as the native display, which the platform
casts to `wl_display*` and dereferences → `wl_proxy_create_wrapper` locks a garbage mutex → SIGSEGV.
So the DRM/GBM backend and the wayland-based `lindroid-drm` EGL platform are **mismatched**: the
platform needs a real `wl_display` from a wayland server providing `android_wlegl`. Fix direction:
run kwin's **wayland backend** (which passes a real `wl_display`) against that server, or ensure the
`android_wlegl` wayland server is reachable via `WAYLAND_DISPLAY` so `wl_display_connect` succeeds.
This — not another libhybris hook — is the real remaining work; the graphics stack itself is fine.

## Status ladder
kernel EVDI open ✅ → libhybris crashes (locale/CFI/stack) ✅ → container Plasma runs ✅ →
unfold so the app draws ✅ → onSurfaceChanged ✅ → create-disp evdi_connect / **Virtual-3
connected** ✅ → **kwin render to EVDI ✗ (crashes in hybris EGL/GBM path)**.

## The working alternative
The **software VNC desktop** ([`vnc.md`](vnc.md)) remains the usable path today — it avoids
libhybris, the EVDI/kwin path, and the crashing DisplayActivity entirely.

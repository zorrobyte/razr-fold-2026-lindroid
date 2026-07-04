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

**The real fix** is libhybris-internals work: port the TLS patcher (`hybris/common/tls_patcher.*`)
to Android 16's bionic — ensure the TLS shadow table is allocated and its base is non-null on
every thread that executes patched bionic code (notably the non-hybris-created main thread that
`create-disp` uses for EGL init). This is an **Android-16 port of libhybris** and belongs
upstream in the Lindroid libhybris fork.

## Status & the working alternative
`create-disp` progresses: kernel EVDI open ✅ → card found/opened ✅ → linker + hooks ✅ →
crashes in EGL init on the libhybris/A16 TLS-patcher bug ⏳. Until that's ported, the
**software VNC desktop** ([`vnc.md`](vnc.md)) is the usable path — it deliberately avoids
libhybris entirely (pure-software mesa), so it sidesteps this whole class of failure.

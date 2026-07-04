# Native (hardware-accelerated) desktop — full diagnosis

Goal: the container's KDE Plasma desktop rendered **on the phone's own screen**, GPU-
accelerated, via `kwin_wayland` → EVDI → the Android composer. This is the "real" Lindroid
convergence path (vs. the software VNC path in [`vnc.md`](vnc.md)).

The path has **three** layered blockers on stock Android 16. Two are fixed; the third is
diagnosed with a patch in progress.

## Layer 1 — kernel: EVDI `open()` → `EINVAL` — **FIXED**

Symptom: black screen. Inside the container:

```
create-disp: evdi-lindroid still not available after add attempt   # restart-loops
kwin_wayland_drm: failed to open drm device .../platform-evdi-lindroid.0-card
```

…and `ls /sys/class/drm` shows dozens of leaked `card*` (create-disp writes
`/sys/devices/evdi-lindroid/add` on every retry).

Root cause: kernel 6.12 `drm_open_helper()` (`drm_file.c:316`):
```c
if (WARN_ON_ONCE(!(filp->f_op->fop_flags & FOP_UNSIGNED_OFFSET)))
    return -EINVAL;
```
The vendored EVDI driver's `evdi_fops` didn't set `FOP_UNSIGNED_OFFSET`, so **every** EVDI
`open()` returned `EINVAL` (real GPU `card0` opened fine).

Fix — [`../patches/kernel/evdi-FOP_UNSIGNED_OFFSET.patch`](../patches/kernel/evdi-FOP_UNSIGNED_OFFSET.patch),
committed to the kernel repo. Verify after flashing:
```bash
# on a clean boot, container not started:
ls /sys/class/drm | grep -c '^card[0-9]*$'      # -> 1 (just the GPU)
echo 1 > /sys/devices/evdi-lindroid/add
# raw open of the new card:  before = EINVAL,  after = fd
```

## Layer 2 — userspace: create-disp runs as `lindroid`, opens the card — OK
Once the kernel opens EVDI, `create-disp` (systemd svc, `User=lindroid`, in `video`+`render`
groups) logs `Found evdi-lindroid at /dev/dri/card1`. Good — but then it crashes.

## Layer 3 — libhybris can't do Android-16 gralloc — **the remaining blocker**

`create-disp` allocates the shared display buffer with `hybris_gralloc_allocate()` /
`_lock()` (libhybris's gralloc wrapper) and **SIGSEGVs** — a jump to an unmapped address
with a corrupt stack, i.e. a call through a bad function pointer inside a libhybris
`[anon:.thunks]` trampoline.

`HYBRIS_LOGGING_LEVEL=debug` gives the smoking gun:
```
__hybris_get_hooked_symbol: Could not find a hook for symbol __stack_chk_fail
__hybris_get_hooked_symbol: Could not find a hook for symbol __vsnprintf_chk
__hybris_get_hooked_symbol: Could not find a hook for symbol __read_chk
__hybris_get_hooked_symbol: Could not find a hook for symbol __write_chk
```

Why it crashes:
* A16's bionic gralloc libs (`android.hardware.graphics.mapper@…`, the QTI gralloc) are
  compiled **stack-protected** and **_FORTIFY_SOURCE**.
* libhybris bridges bionic→glibc calls; bionic's TLS `TLS_SLOT_STACK_GUARD` (slot 5)
  overlaps glibc's TLS, so a stack-canary check can **false-positive**.
* On a (false) canary failure the bionic lib calls `__stack_chk_fail`. Lindroid's libhybris
  is forked from a **2020 base** and does **not** hook `__stack_chk_fail`, so the symbol
  resolves to an unhooked stub → jump to garbage → SIGSEGV.

Note this is the same class of failure that crashes **Xorg** in the VNC path (libhybris
loading A14-era HIDL graphics HALs on A16); VNC dodges it by not using libhybris at all
(software mesa). `create-disp`/`kwin` **must** use libhybris (that's the whole GPU path), so
it can't dodge it.

### The fix (in progress)
Add the missing hooks to `hybris/common/hooks.c` and rebuild `libhybris-common`:
* a no-op `__stack_chk_fail` (survive the false canary),
* `__vsnprintf_chk` / `__read_chk` / `__write_chk` mapped to glibc's fortify functions.

Patch: [`../patches/libhybris/hooks-stack_chk-A16.patch`](../patches/libhybris/hooks-stack_chk-A16.patch)
· build recipe: [`../scripts/patch-build-libhybris.sh`](../scripts/patch-build-libhybris.sh).

**Blocker to finishing it:** building libhybris-common needs the `android-headers` build
package (`android-config.h` + sanitized bionic headers), which only ships from Lindroid's
apt repo — and that server drops the transfer repeatedly. Options: mirror/resume the deb,
generate android-headers from an AOSP tree, or build inside the Lindroid image build.

**Caveat:** the no-op `__stack_chk_fail` masks a *false* canary failure. If the underlying
TLS overlap also corrupts real data, more hooks / a proper libhybris TLS fix for A16 may be
needed. In practice this is an **Android-16 port of libhybris** — worth upstreaming to the
Lindroid libhybris fork.

## Recommended path forward
1. Finish the libhybris rebuild (get `android-headers`, apply the patch, ship the new
   `libhybris-common.so.1` into the rootfs).
2. If canary false-positives persist, investigate the bionic/glibc TLS slot layout under
   `HYBRIS_PATCH_TLS` on A16 (the `libtls-padding.so` preload alone was insufficient here).
3. Then: `create-disp` allocates buffers → `kwin_wayland` renders on the EVDI card the kernel
   fix now opens → the Android composer paints them on-screen.

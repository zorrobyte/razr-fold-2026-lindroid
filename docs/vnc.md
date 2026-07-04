# Working VNC desktop (software-rendered)

A usable Linux desktop **today**, bypassing the native EVDI/kwin path.

Run [`../scripts/vnc-desktop.sh`](../scripts/vnc-desktop.sh) inside the container (it
`su`'s to the `lindroid` user): it starts `Xvfb :1` + XFCE + `x11vnc` on port 5901.

From the PC: `adb forward tcp:5901 tcp:5901`, then any VNC viewer → `localhost:5901`,
password `lindroid`. (The container shares the host net namespace, so x11vnc on `0.0.0.0`
is reachable at the Android host's localhost and thus adb-forwardable over USB.)

### Why the specific setup
* **All Xorg servers segfault** if libhybris is in the process — the rootfs globally forces
  `GBM_BACKEND=hybris` + the libhybris EGL glvnd vendor, which drags in A14-era HIDL graphics
  HALs that are incompatible with Android 16 and crash in a libhybris thunk. Fix: force pure
  software — `__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json`,
  `LIBGL_ALWAYS_SOFTWARE=1`, `GALLIUM_DRIVER=llvmpipe`, and unset the hybris GBM/GLX/PRELOAD.
* **XFCE, not KDE Plasma** — the rootfs's Qt6 is Lindroid's custom **Wayland-only** build with
  no `xcb` platform plugin, so Plasma-on-Xvfb can't start. XFCE (GTK/X11) works fine.
* Networking: the container (shared host netns) has no default route in the main table
  (Android uses policy routing) — `ip route add default via <gw> dev wlan0` + set resolv.conf.

Software-rendered (llvmpipe): smooth for a normal desktop, not for heavy 3D. The native
GPU path is [`native-display.md`](native-display.md).

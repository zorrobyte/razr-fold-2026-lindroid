#!/bin/bash
# Software VNC desktop: Xvfb + XFCE (GTK/X11) + x11vnc on :1 / port 5901.
su - lindroid -c 'bash -s' <<'INNER'
# keep libhybris OUT (its A16-incompatible HALs crash Xorg); pure software
unset GBM_BACKEND __GLX_VENDOR_LIBRARY_NAME LD_PRELOAD HYBRIS_PATCH_TLS
export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
export XDG_RUNTIME_DIR=/tmp/xdg-lindroid
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"

mkdir -p ~/.vnc
echo lindroid | vncpasswd -f > ~/.vnc/passwd; chmod 600 ~/.vnc/passwd

pkill -u lindroid Xvfb 2>/dev/null; pkill -u lindroid x11vnc 2>/dev/null
pkill -u lindroid xfce4-session 2>/dev/null; sleep 1
rm -f /tmp/.X1-lock /tmp/.X11-unix/X1

Xvfb :1 -screen 0 1360x768x24 -nolisten tcp > ~/xvfb.log 2>&1 &
sleep 3
export DISPLAY=:1

dbus-launch --exit-with-session startxfce4 > ~/xfce.log 2>&1 &
sleep 8

x11vnc -display :1 -forever -shared -rfbport 5901 -passwd lindroid -bg -o ~/x11vnc.log

sleep 2
echo "=== VNC listening? ==="; ss -tlnp 2>/dev/null | grep 5901 && echo VNC_LISTENING || echo NO
echo "=== xfce running? ==="; pgrep -u lindroid -a xfwm4 | head -1; pgrep -u lindroid -a xfce4-panel | head -1
# capture the desktop to verify it renders
scrot -o /tmp/desktop.png 2>/dev/null && echo "SCROT_OK $(ls -la /tmp/desktop.png)" || echo "scrot failed"
INNER

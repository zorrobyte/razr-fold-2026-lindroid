#!/bin/bash
# Build the hybris libtls-padding.so (fixes glibc Xorg segfault on Android kernel) and test Xvfb.
set -x
export DEBIAN_FRONTEND=noninteractive
ip route add default via 192.168.4.1 dev wlan0 2>/dev/null || true
printf 'nameserver 1.1.1.1\n' > /etc/resolv.conf
apt-get install -y --no-install-recommends g++ >/dev/null 2>&1

ARCH=aarch64-linux-gnu
mkdir -p /tmp/tlsp
cat > /tmp/tlsp/tls-padding.cpp <<'EOF'
// Reserve early TLS space so bionic's TLS slots don't clobber glibc data.
thread_local void * tls_padding[16];
EOF
g++ -std=c++11 -shared -fPIC -o /usr/lib/$ARCH/libtls-padding.so /tmp/tlsp/tls-padding.cpp
echo "built: $(ls -la /usr/lib/$ARCH/libtls-padding.so 2>&1)"

# proper preload script (the rootfs one was incomplete)
cat > /etc/profile.d/ld_preload_tls_padding.sh <<EOF
#!/bin/sh
case "\$LD_PRELOAD" in
    *libtls-padding.so*) return ;;
esac
export HYBRIS_PATCH_TLS=1
export LD_PRELOAD=/usr/lib/$ARCH/libtls-padding.so\${LD_PRELOAD:+:\${LD_PRELOAD}}
EOF

# test Xvfb with the preload
su - lindroid -c 'bash -s' <<'INNER'
pkill -u lindroid Xvfb 2>/dev/null; sleep 1; rm -f /tmp/.X1-lock /tmp/.X11-unix/X1
export LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libtls-padding.so
export HYBRIS_PATCH_TLS=1
Xvfb :1 -screen 0 1360x768x24 -nolisten tcp > ~/xvfb3.log 2>&1 &
sleep 4
if [ -e /tmp/.X11-unix/X1 ]; then echo "XVFB_UP :-)"; else echo "XVFB_DOWN"; fi
tail -5 ~/xvfb3.log
INNER

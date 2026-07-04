#!/system/bin/sh
# Lindroid: late_start service setup
MODDIR=${0%/*}

# Wait for boot completion
until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done

# SELinux permissive (required by Lindroid architecture)
setenforce 0

# lxcbr0 bridge + NAT so containers get networking (no dnsmasq on stock;
# container side uses static IP 10.0.3.x/24, gw 10.0.3.1)
ip link add name lxcbr0 type bridge 2>/dev/null
ip addr add 10.0.3.1/24 dev lxcbr0 2>/dev/null
ip link set lxcbr0 up
echo 1 > /proc/sys/net/ipv4/ip_forward
iptables -t nat -C POSTROUTING -s 10.0.3.0/24 ! -o lxcbr0 -j MASQUERADE 2>/dev/null || \
    iptables -t nat -A POSTROUTING -s 10.0.3.0/24 ! -o lxcbr0 -j MASQUERADE

# start perspectived (registers "perspective" binder service)
# Containers are NOT auto-started (battery) — launch them from the Lindroid app
# or `lxc_start -n default` on demand.
if ! pgrep -x perspectived >/dev/null; then
    /system_ext/bin/perspectived &
fi

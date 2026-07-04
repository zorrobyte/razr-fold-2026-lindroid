#!/system/bin/sh
# Lindroid: replicate init.lindroid.rc (on init / on post-fs-data)
MODDIR=${0%/*}

# cgroup v1 hierarchies needed by lxc
mkdir -p /sys/fs/cgroup/devices && mount -t cgroup -o devices none /sys/fs/cgroup/devices 2>/dev/null
mkdir -p /sys/fs/cgroup/freezer && mount -t cgroup -o freezer none /sys/fs/cgroup/freezer 2>/dev/null
mkdir -p /sys/fs/cgroup/systemd && mount -t cgroup -o none,name=systemd none /sys/fs/cgroup/systemd 2>/dev/null

# data dirs
for d in "" /mnt /lxc /lxc/log /lxc/run /lxc/rootfs /lxc/rootfs_ro /lxc/container; do
    mkdir -p "/data/lindroid$d"
    chown system:system "/data/lindroid$d"
    chmod 0770 "/data/lindroid$d"
done

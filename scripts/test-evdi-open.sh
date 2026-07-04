#!/system/bin/sh
echo "=== clean-boot EVDI cards (container NOT started) ==="
ls /sys/class/drm/ | grep -E '^card[0-9]+$' | sort -V
N0=$(ls /sys/class/drm/ | grep -cE '^card[0-9]+$')
echo "count=$N0"
echo "=== add ONE evdi display ==="
echo 1 > /sys/devices/evdi-lindroid/add
sleep 2
N1=$(ls /sys/class/drm/ | grep -cE '^card[0-9]+$')
echo "count after add=$N1"
echo "=== newest card ==="
NEW=$(ls /sys/class/drm/ | grep -E '^card[0-9]+$' | sort -V | tail -1)
echo "newest=$NEW  driver=$(cat /sys/class/drm/$NEW/device/uevent 2>/dev/null | grep DRIVER)"
echo "=== raw open() test on the new card (kernel level) ==="
/data/local/tmp/openp /dev/dri/$NEW /dev/dri/card0
echo "=== dmesg tail (open errors?) ==="
dmesg | grep -iE 'evdi|drm' | tail -6

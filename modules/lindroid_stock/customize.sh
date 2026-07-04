#!/system/bin/sh
# Set permissions for Lindroid module payload
ui_print "- Lindroid for stock (blanc) -"

set_perm_recursive $MODPATH 0 0 0755 0644
set_perm_recursive $MODPATH/system/system_ext/bin 0 2000 0755 0755
set_perm $MODPATH/system/system_ext/bin/lindroid-lxc-templates/lxc-lindroid 0 0 0755 0755
set_perm_recursive $MODPATH/system/system_ext/lib64 0 0 0755 0644 u:object_r:system_lib_file:s0
ui_print "- Done. Reboot, then install rootfs via LindroidUI or perspective."

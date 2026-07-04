#!/sbin/sh
# Replaces /system/framework/services.jar with a doubly-patched build and
# invalidates the stale dexopt artifacts so ART loads the patched dex from the
# jar directly (no dex2oat needed). Mechanism proven on this build by the
# services_dispcap module.

ui_print "- Lindroid + Display-Cap services.jar patch"
ui_print "- Installing patched services.jar"
set_perm "$MODPATH/system/framework/services.jar" 0 0 0644 u:object_r:system_file:s0

ui_print "- Invalidating stale services oat (odex/vdex/art + fsv_meta)"
OAT="$MODPATH/system/framework/oat/arm64"
mkdir -p "$OAT"
for f in services.odex services.vdex services.art \
         services.odex.fsv_meta services.vdex.fsv_meta services.art.fsv_meta; do
  mknod "$OAT/$f" c 0 0
done
# remove fs-verity meta + profile that reference the original jar hash
mknod "$MODPATH/system/framework/services.jar.fsv_meta" c 0 0 2>/dev/null
mknod "$MODPATH/system/framework/services.jar.prof" c 0 0 2>/dev/null

ui_print "- IMPORTANT: disable the older services_dispcap module (this supersedes it)"
ui_print "- Done. Reboot to apply."

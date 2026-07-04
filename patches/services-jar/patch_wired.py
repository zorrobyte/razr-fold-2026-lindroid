p = "/root/lindroid/sj/out1/com/android/server/WiredAccessoryManager.smali"
s = open(p).read()
anchor = (
    ".method public final updateLocked("
    "Ljava/lang/String;Ljava/lang/String;IZ)V\n"
    "    .registers 16\n"
)
assert anchor in s, "updateLocked signature not found"
guard = (
    anchor
    + "\n"
    + "    # Lindroid: EVDI virtual-display uevents arrive with a null name,\n"
    + "    # NPE-ing Moto's startsWith and crashing system_server. Skip nulls.\n"
    + "    if-nez p1, :lindroid_name_ok\n\n"
    + "    return-void\n\n"
    + "    :lindroid_name_ok\n"
)
s = s.replace(anchor, guard, 1)
open(p, "w").write(s)
print("patched WiredAccessoryManager.updateLocked null-guard")

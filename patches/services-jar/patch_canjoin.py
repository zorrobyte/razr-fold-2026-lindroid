import io
p = "/root/lindroid/sj/out2/com/android/server/pm/PackageManagerServiceUtils.smali"
s = open(p).read()
anchor = (
    ".method public static canJoinSharedUserId("
    "Ljava/lang/String;Landroid/content/pm/SigningDetails;"
    "Lcom/android/server/pm/SharedUserSetting;I)Z\n"
    "    .registers 10\n"
)
assert anchor in s, "canJoinSharedUserId signature not found"
inject = (
    anchor
    + "\n"
    + "    # Allow any package to join a shared UID regardless of signature\n"
    + "    # (rooted dev device). Scoped to shared-UID joining only.\n"
    + "    const/4 v0, 0x1\n\n"
    + "    return v0\n"
)
s = s.replace(anchor, inject, 1)
open(p, "w").write(s)
print("canJoinSharedUserId -> always true")

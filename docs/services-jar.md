# Patching stock services.jar

Two patches, same pipeline (reuse the `services_dispcap` mechanism from
`razr-fold-2026-display`): baksmali/smali 2.5.2 at **dex 039** (`smali a -a 30`), repack the
dex STORED + `zipalign -p 4`, ship the jar in a Magisk module, and **whiteout** the stale
`oat/arm64/services.{odex,vdex,art}` (+ `.fsv_meta`, `.jar.prof`) with `mknod c 0 0` so ART
loads the patched dex directly. `services_lindroid` supersedes `services_dispcap`.

### 1. Let org.lindroid.ui join android.uid.system
`patches/services-jar/patch_canjoin.py` — force
`PackageManagerServiceUtils.canJoinSharedUserId()` to return true. A *surgical* per-package
exemption in `ReconcilePackageUtils` does **not** work: letting the test-key app "join"
overwrites the shared user's `mSigningDetails` to the test-key, and the next platform package
(`vendor.qti.frameworks.utils`) then fails. Global `canJoinSharedUserId=true` is the robust
fix (scoped to shared-UID joining, not general APK verification).

### 2. WiredAccessoryManager null-guard
`patches/services-jar/patch_wired.py` — the EVDI hotplug uevent reaches Moto's customized
`WiredAccessoryManager.updateLocked(String,String,int,boolean)` with a null name;
`startsWith("soc:qcom,msm-ext-disp")` NPEs → `system_server` dies → RescueParty "Rollback
staged install" reboot. Guard the method to `return` on a null name (a nameless uevent is
never a wired audio accessory, so this is safe).

### Recovery note
Magisk auto-disables **all** modules after ~2 consecutive bootloops (safe mode). `su` isn't
on PATH then — use `/debug_ramdisk/su` and `rm /data/adb/modules/*/disable`.

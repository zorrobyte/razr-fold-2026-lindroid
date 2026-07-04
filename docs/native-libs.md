# A16 native libraries (AOSP/Soong build)

The community prebuilt `libjni_lindroidui.so` is built for Android 14 and **segfaults on
Android 16** (`ComposerImpl::onSurfaceChanged` — the `Surface`/`SurfaceControl` ABI moved).
Rebuild these against A16 from `Linux-on-droid/vendor_lindroid` (`lindroid-22.1`) +
`Linux-on-droid/libhybris` (`lindroid-drm`):

* `libjni_lindroidui.so`  (the app's HWComposer-emulation HAL / JNI)
* `vendor.lindroid.composer-ndk.so`  (the composer AIDL; import `graphics.common-V7` to match
  the platform build and avoid the "multiple versions of the same aidl_interface" error)
* `libhwc2_compat_layer.so`, `libui_compat_layer.so`  (libhybris `compat/apphwc` + `compat/ui`;
  `libui_compat_layer` needs `-DANDROID_VERSION_MAJOR=16` for the A16 `GraphicBuffer` API)

Build in an AOSP `android-16.0.0_r4` tree (`lunch aosp_arm64-trunk_staging-userdebug`,
`m libjni_lindroidui vendor.lindroid.composer-ndk`). Drop the resulting `.so`s into the
`lindroid_stock` Magisk module's `system_ext/lib64` (and bundle `graphics.common-V7-ndk.so`,
since the app's namespace can't reach the platform copy). Because the container bind-mounts
the raw stock partitions, change those mounts to **`rbind`** so the Magisk-overlaid libs are
visible inside the container.

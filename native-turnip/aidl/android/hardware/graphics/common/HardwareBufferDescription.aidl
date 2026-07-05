package android.hardware.graphics.common;
// Wire-compatible subset of common-V5: PixelFormat(@Backing int)->int,
// BufferUsage(@Backing long)->long. Field order matches AHardwareBuffer_Desc.
parcelable HardwareBufferDescription {
    int width;
    int height;
    int layers;
    int format;
    long usage;
    int stride;
}

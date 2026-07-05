// Libhybris-free present bridge for Lindroid on Android 16.
// Replaces create-disp. Allocates the AHardwareBuffer on the Android side
// (native gralloc, no libhybris), presents it via the already-running
// vendor.lindroid.composer IComposer.setBuffer.
//
// Mode --selftest <displayId> [w h]: allocate + CPU-fill a color, present in a
//   loop. Proves the Android->composer->SurfaceFlinger->display path with an
//   Android-allocated buffer (no create-disp, no libhybris).
//
// (Container dma-buf render mode is layered on top once this is proven.)

#include <aidl/vendor/lindroid/composer/IComposer.h>
#include <aidl/android/hardware/graphics/common/HardwareBuffer.h>
#include <aidl/android/hardware/common/NativeHandle.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/hardware_buffer.h>

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <unistd.h>
#include <vector>

// native_handle_t layout (not in NDK); AHardwareBuffer_getNativeHandle is a VNDK symbol.
typedef struct native_handle { int version; int numFds; int numInts; int data[0]; } native_handle_t;
extern "C" const native_handle_t* AHardwareBuffer_getNativeHandle(const AHardwareBuffer*);

using aidl::vendor::lindroid::composer::IComposer;
using aidl::android::hardware::graphics::common::HardwareBuffer;
using aidl::android::hardware::graphics::common::HardwareBufferDescription;
using aidl::android::hardware::common::NativeHandle;

static std::shared_ptr<IComposer> getComposer() {
    // Registered via AServiceManager_addService(binder, "vendor.lindroid.composer").
    for (int i = 0; i < 25; i++) {
        ndk::SpAIBinder b(AServiceManager_getService("vendor.lindroid.composer"));
        if (b.get()) {
            auto c = IComposer::fromBinder(b);
            if (c) return c;
        }
        usleep(200 * 1000);
    }
    return nullptr;
}

// Build an aidl HardwareBuffer that wraps an AHardwareBuffer's gralloc handle.
static bool wrapAHB(AHardwareBuffer* ahb, HardwareBuffer* out) {
    AHardwareBuffer_Desc d; AHardwareBuffer_describe(ahb, &d);
    const native_handle_t* h = AHardwareBuffer_getNativeHandle(ahb);
    if (!h) { fprintf(stderr, "getNativeHandle NULL\n"); return false; }
    out->description.width  = (int)d.width;
    out->description.height = (int)d.height;
    out->description.layers = (int)d.layers;
    out->description.format = (int)d.format;
    out->description.usage  = (long)d.usage;
    out->description.stride = (int)d.stride;
    out->handle.fds.clear();
    out->handle.ints.clear();
    for (int i = 0; i < h->numFds; i++) {
        int fd = dup(h->data[i]);
        out->handle.fds.push_back(ndk::ScopedFileDescriptor(fd));
    }
    for (int i = 0; i < h->numInts; i++)
        out->handle.ints.push_back(h->data[h->numFds + i]);
    return true;
}

int main(int argc, char** argv) {
    ABinderProcess_startThreadPool();
    if (argc < 3 || strcmp(argv[1], "--selftest") != 0) {
        fprintf(stderr, "usage: %s --selftest <displayId> [w h]\n", argv[0]);
        return 2;
    }
    int64_t displayId = atoll(argv[2]);
    uint32_t W = argc > 4 ? atoi(argv[3]) : 1280;
    uint32_t H = argc > 4 ? atoi(argv[4]) : 720;

    auto composer = getComposer();
    if (!composer) { fprintf(stderr, "no composer service\n"); return 2; }
    fprintf(stderr, "got composer; display=%lld %ux%u\n", (long long)displayId, W, H);

    AHardwareBuffer_Desc desc = {};
    desc.width = W; desc.height = H; desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE
               | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER
               | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN
               | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
    AHardwareBuffer* ahb = nullptr;
    if (AHardwareBuffer_allocate(&desc, &ahb) != 0 || !ahb) {
        fprintf(stderr, "AHardwareBuffer_allocate failed\n"); return 2;
    }
    AHardwareBuffer_Desc got; AHardwareBuffer_describe(ahb, &got);
    fprintf(stderr, "allocated AHB stride=%u format=%u usage=%llu\n",
            got.stride, got.format, (unsigned long long)got.usage);

    // CPU-fill a solid color (magenta) so we can see it on-screen.
    for (int frame = 0; frame < 600; frame++) {
        void* addr = nullptr;
        if (AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &addr) == 0 && addr) {
            uint8_t* p = (uint8_t*)addr;
            uint8_t r = (frame*3)&0xff, g = 0x20, b = 0xff - ((frame*3)&0xff);
            for (uint32_t y = 0; y < got.height; y++) {
                uint8_t* row = p + (size_t)y * got.stride * 4;
                for (uint32_t x = 0; x < got.width; x++) {
                    row[x*4+0]=r; row[x*4+1]=g; row[x*4+2]=b; row[x*4+3]=0xff;
                }
            }
            AHardwareBuffer_unlock(ahb, nullptr);
        } else { fprintf(stderr, "lock failed\n"); }

        HardwareBuffer hb;
        if (!wrapAHB(ahb, &hb)) return 2;
        int32_t ret = -1;
        ndk::ScopedFileDescriptor noFence; // null acquire fence
        auto st = composer->setBuffer(displayId, hb, noFence, &ret);
        if (!st.isOk()) {
            fprintf(stderr, "setBuffer transaction failed: %s\n", st.getDescription().c_str());
            return 2;
        }
        if (frame % 60 == 0) fprintf(stderr, "frame %d setBuffer ret=%d\n", frame, ret);
        usleep(33 * 1000); // ~30fps
    }
    AHardwareBuffer_release(ahb);
    return 0;
}

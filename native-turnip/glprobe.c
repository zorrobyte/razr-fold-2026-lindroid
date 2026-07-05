// GL-on-EVDI probe: open a DRM device, gbm+EGL surfaceless GLES context,
// report GL_RENDERER. Proves whether GPU GL works on the EVDI gbm path.
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char** argv){
    const char* dev = argc>1 ? argv[1] : "/dev/dri/card1";
    int surfaceless = (argc>1 && strcmp(argv[1],"surfaceless")==0);
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPD =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy;
    if(surfaceless){
        printf("mode: surfaceless\n");
        dpy = getPD ? getPD(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)
                    : eglGetDisplay(EGL_DEFAULT_DISPLAY);
        goto have_dpy;
    }
    int fd = open(dev, O_RDWR|O_CLOEXEC);
    if(fd<0){ perror("open"); return 2; }
    struct gbm_device* gbm = gbm_create_device(fd);
    if(!gbm){ fprintf(stderr,"gbm_create_device(%s) failed\n",dev); return 2; }
    printf("device: %s  gbm backend: %s\n", dev, gbm_device_get_backend_name(gbm));

    dpy = getPD ? getPD(EGL_PLATFORM_GBM_KHR, gbm, NULL)
                : eglGetDisplay((EGLNativeDisplayType)gbm);
have_dpy:;
    if(dpy==EGL_NO_DISPLAY){ fprintf(stderr,"eglGetPlatformDisplay failed\n"); return 2; }
    EGLint maj,min;
    if(!eglInitialize(dpy,&maj,&min)){ fprintf(stderr,"eglInitialize failed 0x%x\n",eglGetError()); return 2; }
    printf("EGL %d.%d  EGL_VENDOR=%s\n", maj, min, eglQueryString(dpy,EGL_VENDOR));

    if(!eglBindAPI(EGL_OPENGL_ES_API)){ fprintf(stderr,"bindAPI failed\n"); return 2; }
    EGLint ctxattr[]={ EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctxattr);
    if(ctx==EGL_NO_CONTEXT){ fprintf(stderr,"eglCreateContext failed 0x%x\n",eglGetError()); return 2; }
    if(!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)){
        fprintf(stderr,"eglMakeCurrent(surfaceless) failed 0x%x\n",eglGetError()); return 2;
    }
    const char* rend=(const char*)glGetString(GL_RENDERER);
    const char* ver =(const char*)glGetString(GL_VERSION);
    const char* vnd =(const char*)glGetString(GL_VENDOR);
    printf("GL_RENDERER = %s\n", rend?rend:"(null)");
    printf("GL_VERSION  = %s\n", ver?ver:"(null)");
    printf("GL_VENDOR   = %s\n", vnd?vnd:"(null)");
    // simple GPU exercise: clear + read one pixel via a 1x1 FBO is overkill; RENDERER string is the proof.
    int sw = rend && (strstr(rend,"llvmpipe")||strstr(rend,"softpipe")||strstr(rend,"swrast"));
    printf("RESULT: %s\n", (rend && !sw) ? "GPU" : "SOFTWARE/FAIL");
    return (rend && !sw) ? 0 : 1;
}

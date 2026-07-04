#!/bin/bash
set -e
cd /root/libhybris-src
# fetch submodules (android-headers) needed to build
git submodule update --init --recursive 2>&1 | tail -3 || true

cd hybris/common
python3 - <<'PY'
p="hooks.c"
s=open(p).read()
# 1) add hook functions right after the __snprintf_chk hook, before the tls_hooks decl
anchor="""static __attribute__((tls_model ("initial-exec")))
       __thread void *tls_hooks[16];"""
assert anchor in s, "tls_hooks anchor not found"
funcs="""/* Android 16: bionic gralloc libs are stack-protected; under libhybris the
 * bionic TLS stack-guard slot overlaps glibc TLS so a canary check can
 * false-positive. Provide a no-op __stack_chk_fail so it doesn't resolve to an
 * unhooked stub and jump to garbage. Also expose the fortify read/write/vsnprintf
 * variants (glibc has them). */
static void _hybris_hook___stack_chk_fail(void)
{
}

extern int __vsnprintf_chk(char *, size_t, int, size_t, const char *, __gnuc_va_list);
extern long __read_chk(int, void *, size_t, size_t);
extern long __write_chk(int, const void *, size_t, size_t);

"""
s=s.replace(anchor, funcs+anchor, 1)
# 2) add table entries after HOOK_INDIRECT(__snprintf_chk),
tanchor="    HOOK_INDIRECT(__snprintf_chk),"
assert tanchor in s, "table anchor not found"
entries=tanchor+"""
    HOOK_TO(__stack_chk_fail, _hybris_hook___stack_chk_fail),
    HOOK_TO(__vsnprintf_chk, __vsnprintf_chk),
    HOOK_TO(__read_chk, __read_chk),
    HOOK_TO(__write_chk, __write_chk),"""
s=s.replace(tanchor, entries, 1)
open(p,"w").write(s)
print("hooks.c patched")
PY

cd /root/libhybris-src/hybris
echo "=== autogen + configure ==="
NOCONFIGURE=1 ./autogen.sh >/tmp/hyb-conf.log 2>&1 || true
# mirror the lindroid build: enable arch + wayland off, use system android-headers
./configure --prefix=/usr --libdir=/usr/lib/aarch64-linux-gnu \
   --with-android-headers=/root/libhybris-src/android-headers \
   --enable-arch=arm64 --enable-mali-quirks --disable-trace >/tmp/hyb-conf.log 2>&1 \
   && echo "configure OK" || { echo "configure FAILED"; tail -20 /tmp/hyb-conf.log; exit 1; }
echo "=== build libhybris-common only ==="
make -j8 -C common libhybris-common.la >/tmp/hyb-make.log 2>&1 \
   && echo "BUILD OK" || { echo "BUILD FAILED"; tail -25 /tmp/hyb-make.log; exit 1; }
echo "=== built lib ==="
find . -name "libhybris-common.so*" 2>/dev/null | head

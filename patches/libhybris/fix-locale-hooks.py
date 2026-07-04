#!/usr/bin/env python3
# Hook bionic's thread-local locale accessors to glibc's. On a glibc thread the
# bionic TLS (thread-local locale) is null, so these bionic functions deref NULL
# (crash: __ctype_get_mb_cur_max -> ldr [bionic_tls+0x820], bionic_tls=NULL).
# glibc exports all of these, so redirect them.
import sys
p = "/root/libhybris-src/hybris/common/hooks.c"
s = open(p).read()

MARK = "_hybris_hook___vsnprintf_chk"  # our chk block is present
if "HOOK_TO(__ctype_get_mb_cur_max" in s:
    print("locale hooks already present"); sys.exit(0)

# 1) externs: add after the chk wrapper impls (before the tls_hooks anchor)
anchor = "static __attribute__((tls_model (\"initial-exec\")))\n       __thread void *tls_hooks[16];"
externs = '''/* Android 16: bionic thread-local locale accessors deref bionic TLS, which is
 * NULL on glibc threads (e.g. create-disp's EGL init). Redirect to glibc, which
 * has all of these. */
extern unsigned long __ctype_get_mb_cur_max(void);
extern const unsigned short **__ctype_b_loc(void);
extern const int **__ctype_tolower_loc(void);
extern const int **__ctype_toupper_loc(void);

'''
assert anchor in s, "tls_hooks anchor not found"
s = s.replace(anchor, externs + anchor, 1)

# 2) table entries after our __write_chk hook
tanchor = "    HOOK_TO(__write_chk, _hybris_hook___write_chk),"
assert tanchor in s, "write_chk table anchor not found"
entries = tanchor + '''
    HOOK_TO(__ctype_get_mb_cur_max, __ctype_get_mb_cur_max),
    HOOK_TO(__ctype_b_loc, __ctype_b_loc),
    HOOK_TO(__ctype_tolower_loc, __ctype_tolower_loc),
    HOOK_TO(__ctype_toupper_loc, __ctype_toupper_loc),'''
s = s.replace(tanchor, entries, 1)

open(p, "w").write(s)
print("locale hooks added")

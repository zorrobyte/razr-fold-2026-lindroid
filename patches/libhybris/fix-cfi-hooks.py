#!/usr/bin/env python3
# bionic Control Flow Integrity: __cfi_slowpath reads the __cfi_shadow map, which
# the hybris linker never sets up -> null deref (ldrh w8,[x9,x8], x9=__cfi_shadow=NULL).
# CFI is a cross-DSO indirect-call security check; under libhybris it can't work.
# No-op it (skip the check) so bionic graphics libs run.
import sys
p="/root/libhybris-src/hybris/common/hooks.c"
s=open(p).read()
if "HOOK_TO(__cfi_slowpath" in s:
    print("cfi hooks already present"); sys.exit(0)

anchor='static __attribute__((tls_model ("initial-exec")))\n       __thread void *tls_hooks[16];'
impl='''/* Android bionic CFI shadow isn't initialized under the hybris linker, so
 * __cfi_slowpath derefs a null shadow map. CFI is a security check on cross-DSO
 * indirect calls; no-op it so the check is skipped. */
static void _hybris_hook___cfi_slowpath(uint64_t CallSiteTypeId, void *Ptr)
{
    (void) CallSiteTypeId; (void) Ptr;
}
static void _hybris_hook___cfi_slowpath_diag(uint64_t CallSiteTypeId, void *Ptr, void *DiagData)
{
    (void) CallSiteTypeId; (void) Ptr; (void) DiagData;
}

'''
assert anchor in s, "tls_hooks anchor not found"
s=s.replace(anchor, impl+anchor, 1)

tanchor="    HOOK_TO(__write_chk, _hybris_hook___write_chk),"
entries=tanchor+'''
    HOOK_TO(__cfi_slowpath, _hybris_hook___cfi_slowpath),
    HOOK_TO(__cfi_slowpath_diag, _hybris_hook___cfi_slowpath_diag),'''
assert tanchor in s, "write_chk table anchor not found"
s=s.replace(tanchor, entries, 1)
open(p,"w").write(s)
print("cfi hooks added")

#!/usr/bin/env python3
# Fixup: replace the extern __*_chk decls (which assume glibc provides them --
# glibc has __read_chk/__vsnprintf_chk but NOT __write_chk) with local wrapper
# implementations, and repoint the hook-table entries at them.
import re, sys
p = "/root/libhybris-src/hybris/common/hooks.c"
s = open(p).read()

old_externs = '''extern int __vsnprintf_chk(char *, size_t, int, size_t, const char *, __gnuc_va_list);
extern long __read_chk(int, void *, size_t, size_t);
extern long __write_chk(int, const void *, size_t, size_t);'''

new_impls = '''static ssize_t _hybris_hook___write_chk(int fd, const void *buf, size_t nbytes, size_t buflen)
{
    if (nbytes > buflen)
        _hybris_hook___stack_chk_fail();
    return write(fd, buf, nbytes);
}

static ssize_t _hybris_hook___read_chk(int fd, void *buf, size_t nbytes, size_t buflen)
{
    if (nbytes > buflen)
        _hybris_hook___stack_chk_fail();
    return read(fd, buf, nbytes);
}

static int _hybris_hook___vsnprintf_chk(char *s, size_t maxlen, int flag, size_t slen,
                                        const char *format, va_list ap)
{
    (void) flag; (void) slen;
    return vsnprintf(s, maxlen, format, ap);
}'''

if old_externs in s:
    s = s.replace(old_externs, new_impls, 1)
    print("externs -> local impls: OK")
elif "_hybris_hook___write_chk" in s:
    print("already fixed up")
else:
    print("ERROR: extern block not found"); sys.exit(1)

# repoint table entries
repl = [
    ("HOOK_TO(__vsnprintf_chk, __vsnprintf_chk),", "HOOK_TO(__vsnprintf_chk, _hybris_hook___vsnprintf_chk),"),
    ("HOOK_TO(__read_chk, __read_chk),",           "HOOK_TO(__read_chk, _hybris_hook___read_chk),"),
    ("HOOK_TO(__write_chk, __write_chk),",         "HOOK_TO(__write_chk, _hybris_hook___write_chk),"),
]
for a, b in repl:
    if a in s:
        s = s.replace(a, b, 1); print("table:", a, "->", b)
    elif b in s:
        print("table already:", b)
    else:
        print("WARN table entry missing:", a)

open(p, "w").write(s)
print("DONE-FIX")

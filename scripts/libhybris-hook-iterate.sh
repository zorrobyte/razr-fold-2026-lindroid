#!/bin/bash
# One whack-a-mole iteration: apply a hooks.c patch (arg $1 = python script),
# rebuild libhybris-common (production), install, retest create-disp, report the
# NEXT crash's function.
set -e
export LC_ALL=C
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export HOME=/root
unset TMPDIR
LIBDIR=/usr/lib/aarch64-linux-gnu
LIBC=/apex/com.android.runtime/lib64/bionic/libc.so

echo "=== apply patch: $1 ==="
python3 "/root/$1"

echo "=== rebuild libhybris-common ==="
cd /root/libhybris-src/hybris
make -j"$(nproc)" -C common >/tmp/hi.log 2>&1 && echo "BUILD OK" || { echo "BUILD FAIL"; grep -iE "error:|undefined|No such" /tmp/hi.log | tail -20; exit 1; }
install -m0644 common/.libs/libhybris-common.so.1.0.0 "$LIBDIR/libhybris-common.so.1"
ldconfig 2>/dev/null || true

echo "=== retest create-disp ==="
set +e
timeout 15 runuser -u lindroid -- env EGL_PLATFORM=lindroid-drm create-disp >/tmp/cdi.log 2>&1
RC=$?
echo "create-disp exit=$RC  (124=SURVIVED 15s = WIN)"
echo "--- create-disp output ---"; tail -6 /tmp/cdi.log
echo "--- /dev/dri after ---"; ls /dev/dri/ 2>/dev/null

if [ $RC -eq 139 ]; then
  echo "=== still crashing -> resolve NEXT function ==="
  gdb -q -batch -ex 'set pagination off' -ex 'run' \
    -ex 'printf "CRASHPC %#lx\n", $pc' -ex 'x/3i $pc' -ex 'info proc mappings' \
    --args create-disp 2>/dev/null > /tmp/nx.log
  python3 - "$LIBC" <<'PY'
import re,sys,subprocess
libc=sys.argv[1]; t=open('/tmp/nx.log').read()
m=re.search(r'CRASHPC (0x[0-9a-f]+)',t)
if not m: print("no crashpc"); sys.exit()
pc=int(m.group(1),16); print("next crash pc",hex(pc))
for mm in re.finditer(r'^\s*(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+0x[0-9a-f]+\s+(0x[0-9a-f]+)\s+\S+\s+(\S+)?',t,re.M):
    s,e,off,path=int(mm.group(1),16),int(mm.group(2),16),int(mm.group(3),16),(mm.group(4) or '')
    if s<=pc<e:
        fo=pc-s+off; print(f"in {path} fileoff={hex(fo)}")
        if path==libc:
            nm=subprocess.run(["nm","-D","--defined-only","-n",libc],capture_output=True,text=True).stdout
            last=None
            for ln in nm.splitlines():
                p=ln.split()
                if len(p)>=3 and p[1] in ("T","t","W","w","i"):
                    try:a=int(p[0],16)
                    except:continue
                    if a<=fo: last=(a,p[2])
                    else: break
            if last: print(f"  -> nearest sym: {last[1]} +{hex(fo-last[0])}")
        break
# show the crash instruction
for ln in t.splitlines():
    if '=>' in ln: print("instr:",ln.strip())
PY
fi
echo "DONE-ITER"

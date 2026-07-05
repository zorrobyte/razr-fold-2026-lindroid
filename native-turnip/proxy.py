import socket, threading, sys, select

LISTEN = ("127.0.0.1", 8899)

def pipe(a, b):
    try:
        while True:
            r, _, _ = select.select([a, b], [], [], 120)
            if not r:
                break
            for s in r:
                data = s.recv(65536)
                if not data:
                    return
                (b if s is a else a).sendall(data)
    except Exception:
        pass
    finally:
        for s in (a, b):
            try: s.close()
            except Exception: pass

def handle(client):
    try:
        client.settimeout(60)
        buf = b""
        while b"\r\n\r\n" not in buf:
            d = client.recv(4096)
            if not d:
                client.close(); return
            buf += d
            if len(buf) > 131072:
                break
        head, _, rest = buf.partition(b"\r\n\r\n")
        lines = head.split(b"\r\n")
        req = lines[0].split()
        if len(req) < 3:
            client.close(); return
        method, target = req[0], req[1]
        if method == b"CONNECT":
            host, _, port = target.decode().partition(":")
            up = socket.create_connection((host, int(port or 443)), timeout=30)
            client.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
            pipe(client, up)
        else:
            t = target.decode()
            if t.startswith("http://"):
                hostpath = t[7:]
                host, _, path = hostpath.partition("/")
                path = "/" + path
            else:
                host, path = "", t
                for h in lines[1:]:
                    if h.lower().startswith(b"host:"):
                        host = h.split(b":", 1)[1].strip().decode()
            port = 80
            if ":" in host:
                host, _, p = host.partition(":"); port = int(p)
            newlines = [("%s %s HTTP/1.1" % (method.decode(), path)).encode()]
            for h in lines[1:]:
                lh = h.lower()
                if lh.startswith(b"proxy-connection") or lh.startswith(b"connection"):
                    continue
                newlines.append(h)
            newlines.append(b"Connection: close")
            newreq = b"\r\n".join(newlines) + b"\r\n\r\n" + rest
            up = socket.create_connection((host, port), timeout=30)
            up.sendall(newreq)
            pipe(client, up)
    except Exception:
        try: client.close()
        except Exception: pass

def main():
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(LISTEN); s.listen(256)
    sys.stderr.write("proxy up on %s:%d\n" % LISTEN); sys.stderr.flush()
    while True:
        c, _ = s.accept()
        threading.Thread(target=handle, args=(c,), daemon=True).start()

main()

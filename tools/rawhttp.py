#!/usr/bin/env python3
"""Raw-socket HTTP client: timestamp every recv() chunk to expose TCP segmentation stalls."""
import socket, sys, time

HOST = "esp-ot-br.local"
PORT = 80
PATH = sys.argv[1] if len(sys.argv) > 1 else "/web/features"
N = int(sys.argv[2]) if len(sys.argv) > 2 else 3


def once(sock, path, tag):
    req = ("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n\r\n" % (path, HOST)).encode()
    t0 = time.time()
    sock.sendall(req)
    t_sent = time.time()
    chunks = []
    total = 0
    while True:
        sock.settimeout(15)
        try:
            b = sock.recv(65536)
        except socket.timeout:
            chunks.append((time.time() - t0, -1, b"<TIMEOUT>"))
            break
        if not b:
            chunks.append((time.time() - t0, 0, b"<EOF>"))
            break
        t = time.time() - t0
        chunks.append((t, len(b), b))
        total += len(b)
        if b"\r\n\r\n" in b"".join(c[2] for c in chunks):
            # headers seen; check if we also have the full content-length body
            joined = b"".join(c[2] for c in chunks)
            head, _, body = joined.partition(b"\r\n\r\n")
            cl = 0
            for line in head.split(b"\r\n"):
                if line.lower().startswith(b"content-length:"):
                    cl = int(line.split(b":")[1])
            if len(body) >= cl:
                break
    print("--- %s  (send took %.3fs) ---" % (tag, t_sent - t0))
    for t, n, b in chunks:
        prev = 0
        show = b[:70].replace(b"\r\n", b"\\r\\n")
        print("   t+%7.3fs  %5s bytes  %s" % (t, n if n >= 0 else "TO", show))
    print("   total %d bytes" % total)
    return total


s = socket.create_connection((HOST, PORT), timeout=20)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
for i in range(N):
    once(s, PATH, "%s #%d" % (PATH, i + 1))
s.close()

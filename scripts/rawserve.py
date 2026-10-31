#!/usr/bin/env python3
"""Raw TCP file streamer.

Serves the raw bytes of a file to each TCP client that connects, then closes.
Useful to flash an image onto a device that has bash but no wget/curl/busybox:
the target reads the socket with bash and writes straight to a block device, e.g.

    # on this PC (LAN):
    python3 rawserve.py 9000 armbian.img            # binds 0.0.0.0:9000
    # on the target (bash shell, as root):
    cat < /dev/tcp/<pc-ip>/9000 > /dev/mmcblk2 ; sync

Serve the DECOMPRESSED image so the target doesn't need an xz decompressor.

Usage: rawserve.py <port> <file> [bind-host]   (bind-host default 0.0.0.0)
"""
import socket, sys, os

if len(sys.argv) < 3:
    sys.exit(__doc__)
port = int(sys.argv[1])
path = sys.argv[2]
host = sys.argv[3] if len(sys.argv) > 3 else "0.0.0.0"

s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((host, port))
s.listen(1)
print(f"serving {path} ({os.path.getsize(path)} bytes) on {host}:{port}", flush=True)
while True:
    c, a = s.accept()
    print("conn from", a, flush=True)
    n = 0
    try:
        with open(path, "rb") as f:
            while True:
                b = f.read(1 << 20)
                if not b:
                    break
                c.sendall(b)
                n += len(b)
        print("sent", n, "bytes", flush=True)
    except Exception as e:
        print("err", e, flush=True)
    finally:
        try:
            c.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        c.close()

#!/usr/bin/env python3
"""
The proxy must survive status clients that hang up mid-reply (SIGPIPE).
Opens the status socket hundreds of times and resets each connection
immediately (SO_LINGER 0), so the proxy writes into a closed socket.
usage: robustness-test.py <igmpproxy binary>   (Linux, root)
"""
import os
import socket
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nstest import Proc, ns, setup, teardown  # noqa: E402

SOCK = "/var/run/igmpproxy.sock"


def main():
    binary = sys.argv[1]
    tmp = tempfile.mkdtemp(prefix="rob-")
    conf = os.path.join(tmp, "c.conf")
    with open(conf, "w") as f:
        f.write("phyint u0 upstream\n  altnet 10.0.1.0/24\nphyint br0 downstream\nphyint lo disabled\n")
    setup()
    proxy = None
    try:
        proxy = Proc(f"nsenter --net=/run/netns/rtr {binary} -d {conf}", os.path.join(tmp, "proxy.log"))
        time.sleep(1)
        for _ in range(300):
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                s.connect(SOCK)
                s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            except OSError:
                pass
            s.close()
        time.sleep(1)
        alive = proxy.p.poll() is None
        ok = alive and ns("rtr", f"{binary} -S {SOCK}").startswith("{")
    finally:
        if proxy:
            proxy.kill()
        time.sleep(0.3)
        teardown()
    print("proxy survived 300 aborted status connections:", "yes" if ok else "NO")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

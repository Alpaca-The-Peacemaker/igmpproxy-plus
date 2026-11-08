#!/usr/bin/env python3
"""
Link-local groups (224.0.0.0/24, e.g. mDNS 224.0.0.251) must never be
proxied: a receiver joining one must not create a group or an upstream join.
usage: linklocal-test.py <igmpproxy binary>   (Linux, root)
"""
import json
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nstest import GROUP, Proc, ns, setup, teardown  # noqa: E402

JOIN = """
import socket, struct, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for g in ("224.0.0.251", "%s"):
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                 struct.pack("4s4s", socket.inet_aton(g), socket.inet_aton("0.0.0.0")))
time.sleep(60)
""" % GROUP


def main():
    binary = sys.argv[1]
    tmp = tempfile.mkdtemp(prefix="ll-")
    conf = os.path.join(tmp, "c.conf")
    with open(conf, "w") as f:
        f.write("phyint u0 upstream\n  altnet 10.0.1.0/24\nphyint br0 downstream\nphyint lo disabled\n")
    setup()
    procs = []
    try:
        procs.append(Proc(f"nsenter --net=/run/netns/rtr {binary} -d -v -v {conf}", os.path.join(tmp, "proxy.log")))
        time.sleep(1)
        procs.append(Proc(f"nsenter --net=/run/netns/rx1 python3 -c '{JOIN}'", os.devnull))
        time.sleep(4)
        st = json.loads(ns("rtr", f"{binary} -S /var/run/igmpproxy.sock"))
        groups = [g["group"] for g in st["groups"]]
    finally:
        for p in procs:
            p.kill()
        time.sleep(0.3)
        teardown()
    print("groups:", groups)
    ok = GROUP in groups and not any(g.startswith("224.0.0.") for g in groups)
    print("link-local test", "passed" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

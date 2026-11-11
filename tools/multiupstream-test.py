#!/usr/bin/env python3
"""
Two upstream interfaces (e.g. internet and an IPTV VLAN), each with its own
group whitelist and both with altnet 0.0.0.0/0 (sources can be anywhere):

  [ns srcA] 10.0.1.2 --- u0 10.0.1.1 [ns rtr] br0 10.0.2.1 --- 10.0.2.11 [ns rx1]
  [ns srcB] 10.0.3.2 --- u1 10.0.3.1 [ns rtr]

  u0: whitelist 239.1.1.0/24     u1: whitelist 239.2.2.0/24

A receiver joins 239.1.1.1, 239.2.2.1 and 239.3.3.1 (in neither list).
Expected: 239.1.1.1 joined only on u0 and forwarded from srcA, 239.2.2.1
only on u1 and forwarded from srcB, 239.3.3.1 joined nowhere. Before 1.6.0
the proxy picked the upstream for a flow from its source address, so with
overlapping altnets the u1 stream got a route expecting it on u0 and was
never forwarded.

usage: multiupstream-test.py <igmpproxy binary>   (exit 1 on failure)
"""
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nstest import Proc, ns, sh  # noqa: E402

NS = ("srcA", "srcB", "rtr", "rx1")
GROUPS = ("239.1.1.1", "239.2.2.1", "239.3.3.1")

SEND = """
import socket, sys, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 8)
groups = sys.argv[2:]
while True:
    for g in groups:
        s.sendto(b"x" * 100, (g, 5000))
    time.sleep(0.02)
"""

JOIN = """
import socket, struct, sys, time
socks = []
for g in sys.argv[2:]:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                 struct.pack("4s4s", socket.inet_aton(g), socket.inet_aton("0.0.0.0")))
    socks.append(s)
time.sleep(3600)
"""


def teardown():
    for n in NS:
        sh(f"ip netns del {n}", check=False)
    sh("pkill -f 'mu-(send|join)'", check=False)


def setup():
    teardown()
    for n in NS:
        sh(f"ip netns add {n}")
        ns(n, "ip link set lo up")
    for src, up, net in (("srcA", "u0", "10.0.1"), ("srcB", "u1", "10.0.3")):
        sh(f"ip link add s0 netns {src} type veth peer name {up} netns rtr")
        ns(src, f"ip addr add {net}.2/24 dev s0")
        ns(src, "ip link set s0 up")
        ns(src, f"ip route add default via {net}.1")
        ns("rtr", f"ip addr add {net}.1/24 dev {up}")
        ns("rtr", f"ip link set {up} up")
    sh("ip link add b1 netns rtr type veth peer name r1 netns rx1")
    ns("rtr", "ip addr add 10.0.2.1/24 dev b1")
    ns("rtr", "ip link set b1 up")
    ns("rx1", "ip addr add 10.0.2.11/24 dev r1")
    ns("rx1", "ip link set r1 up")
    ns("rx1", "ip route add default via 10.0.2.1")
    ns("rtr", "sysctl -qw net.ipv4.conf.all.rp_filter=0")
    for i in ("u0", "u1", "b1"):
        ns("rtr", f"sysctl -qw net.ipv4.conf.{i}.rp_filter=0")


def maddr(dev):
    return ns("rtr", f"ip maddr show dev {dev}")


def main():
    binary = sys.argv[1]
    tmp = tempfile.mkdtemp(prefix="multiup-")
    conf = os.path.join(tmp, "igmpproxy.conf")
    with open(conf, "w") as f:
        f.write("controlsocket none\n"
                "phyint u0 upstream\n  altnet 0.0.0.0/0\n  whitelist 239.1.1.0/24\n"
                "phyint u1 upstream\n  altnet 0.0.0.0/0\n  whitelist 239.2.2.0/24\n"
                "phyint b1 downstream\nphyint lo disabled\n")
    problems, procs = [], []
    setup()
    try:
        cap = os.path.join(tmp, "rx1.txt")
        procs.append(Proc("nsenter --net=/run/netns/rx1 tcpdump -n -l -i r1 udp port 5000", cap))
        procs.append(Proc(f"nsenter --net=/run/netns/rtr {binary} -d -v -v {conf}", os.path.join(tmp, "proxy.log")))
        time.sleep(1.5)
        procs.append(Proc(f"nsenter --net=/run/netns/srcA python3 -c '{SEND}' mu-send 239.1.1.1", os.devnull))
        procs.append(Proc(f"nsenter --net=/run/netns/srcB python3 -c '{SEND}' mu-send 239.2.2.1", os.devnull))
        procs.append(Proc(f"nsenter --net=/run/netns/rx1 python3 -c '{JOIN}' mu-join {' '.join(GROUPS)}", os.devnull))
        time.sleep(5)

        u0, u1 = maddr("u0"), maddr("u1")
        for g, on0, on1 in (("239.1.1.1", True, False), ("239.2.2.1", False, True), ("239.3.3.1", False, False)):
            if (g in u0) != on0 or (g in u1) != on1:
                problems.append(f"{g}: joined on u0={g in u0} u1={g in u1}, expected u0={on0} u1={on1}")
        counts = {g: 0 for g in GROUPS}
        with open(cap) as f:
            for line in f:
                for g in GROUPS:
                    if f"> {g}.5000" in line:
                        counts[g] += 1
        print("joined upstream: u0", [g for g in GROUPS if g in u0], "u1", [g for g in GROUPS if g in u1])
        print("packets at the receiver:", counts)
        if counts["239.1.1.1"] < 50:
            problems.append(f"239.1.1.1 from upstream u0 not forwarded ({counts['239.1.1.1']} packets)")
        if counts["239.2.2.1"] < 50:
            problems.append(f"239.2.2.1 from upstream u1 not forwarded ({counts['239.2.2.1']} packets)")
        log = open(os.path.join(tmp, "proxy.log")).read()
        if "is not in any allowed network" in log:
            problems.append("warning about an allowed network although altnet is 0.0.0.0/0")
    finally:
        for p in procs:
            p.kill()
        time.sleep(0.3)
        teardown()

    for p in problems:
        print("FAIL:", p)
    print("multi upstream test passed" if not problems else f"{len(problems)} problems")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()

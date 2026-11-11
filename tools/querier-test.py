#!/usr/bin/env python3
"""
Querier test for igmpproxy-plus (Linux, root). Uses the namespaces of
nstest.py; the router's br0 gets 10.0.2.100 so other routers on the LAN can
have a lower or a higher address.

  1. "querier" role without any upstream: the proxy starts and sends general
     queries; a leave gets group specific queries.
  2. upstream u0 + querier br0: a member on br0 gets no stream and nothing
     is joined upstream.
  3. querierelection on: a querier with a higher address changes nothing;
     one with a lower address silences our general queries (querier_lost),
     and when it stops we query again (querier_resumed).
  4. querierelection off (default): we keep querying next to it.

usage: querier-test.py <igmpproxy binary>   (exit 1 on failure)
"""
import json
import os
import signal
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nstest import GROUP, RECV, SENDER, Proc, ns, setup, teardown  # noqa: E402

SOCK = "/var/run/igmpproxy.sock"
ME = "10.0.2.100"

# Another router: IGMPv2 general queries every 2 s, max response 1 s.
FAKE_QUERIER = """
import socket, struct, sys, time
src = sys.argv[1]
def csum(b):
    s = sum(struct.unpack("!%dH" % (len(b) // 2), b))
    s = (s >> 16) + (s & 0xffff); s += s >> 16
    return ~s & 0xffff
q = struct.pack("!BBH4s", 0x11, 10, 0, bytes(4))
q = struct.pack("!BBH4s", 0x11, 10, csum(q), bytes(4))
s = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_IGMP)
s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(src))
while True:
    s.sendto(q, ("224.0.0.1", 0))
    time.sleep(2)
"""


def start_proxy(tmp, name, lines, binary):
    conf = os.path.join(tmp, name + ".conf")
    with open(conf, "w") as f:
        f.write("\n".join([f"eventlog {tmp}/{name}.jsonl"] + lines) + "\n")
    p = Proc(f"nsenter --net=/run/netns/rtr {binary} -d -v -v {conf}", os.path.join(tmp, name + ".log"))
    time.sleep(1.5)
    return p


def queries(path, src, since=0.0, group=None):
    """Times of IGMP queries from src seen in a tcpdump -tt -n log."""
    out = []
    with open(path) as f:
        for line in f:
            if f"IP {src} > " not in line or "igmp query" not in line:
                continue
            if group is not None and group not in line:
                continue
            if group is None and "gaddr" in line:
                continue          # group specific
            t = float(line.split()[0])
            if t >= since:
                out.append(t)
    return out


def events(binary):
    return [json.loads(l) for l in ns("rtr", f"{binary} -E {SOCK}").splitlines() if l.startswith("{")]


def main():
    binary = sys.argv[1]
    tmp = tempfile.mkdtemp(prefix="querier-")
    problems = []
    setup()
    procs = []
    try:
        ns("rtr", "ip addr flush dev br0")
        ns("rtr", f"ip addr add {ME}/24 dev br0")
        ns("rx3", "ip addr flush dev r3")
        ns("rx3", "ip addr add 10.0.2.200/24 dev r3")
        cap = os.path.join(tmp, "rx1.txt")
        procs.append(Proc("nsenter --net=/run/netns/rx1 tcpdump -tt -n -l -i r1 igmp or udp port 5000", cap))
        upcap = os.path.join(tmp, "up.txt")
        procs.append(Proc("nsenter --net=/run/netns/rtr tcpdump -tt -n -l -i u0 igmp", upcap))
        time.sleep(1)

        # --- 1. querier only, no upstream
        base = ["queryinterval 3s", "queryresponseinterval 1s", "lastmemberinterval 500ms",
                "lastmembercount 2"]
        p = start_proxy(tmp, "q1", base + ["phyint br0 querier", "phyint u0 disabled", "phyint lo disabled"], binary)
        procs.append(p)
        t0 = time.time()
        time.sleep(7)
        if p.p.poll() is not None:
            problems.append("1: proxy with only a querier interface exited: " + open(os.path.join(tmp, "q1.log")).read()[-500:])
        n = len(queries(cap, ME, t0))
        print(f"1: {n} general queries in 7 s (query interval 3 s)")
        if n < 2:
            problems.append(f"1: expected at least 2 general queries, saw {n}")
        st = json.loads(ns("rtr", f"{binary} -S {SOCK}"))
        br = next((i for i in st["interfaces"] if i["name"] == "br0"), {})
        if br.get("state") != "querier" or br.get("querier") != "self":
            problems.append(f"1: status for br0: {br}")
        r = Proc(f"nsenter --net=/run/netns/rx1 python3 -u -c '{RECV}' q-recv", os.path.join(tmp, "r1.log"))
        procs.append(r)
        time.sleep(1.5)
        st = json.loads(ns("rtr", f"{binary} -S {SOCK}"))
        br = next((i for i in st["interfaces"] if i["name"] == "br0"), {})
        if GROUP not in [g["group"] for g in br.get("groups", [])]:
            problems.append(f"1: {GROUP} not in the querier groups: {br.get('groups')}")
        t_leave = time.time()
        os.killpg(r.p.pid, signal.SIGUSR1)
        time.sleep(3)
        st = json.loads(ns("rtr", f"{binary} -S {SOCK}"))
        br = next((i for i in st["interfaces"] if i["name"] == "br0"), {})
        if GROUP in [g["group"] for g in br.get("groups", [])]:
            problems.append(f"1: {GROUP} still listed after the leave: {br.get('groups')}")
        gsq = queries(cap, ME, t_leave, group=GROUP)
        print(f"1: {len(gsq)} group specific queries after the leave")
        if len(gsq) != 2:
            problems.append(f"1: expected 2 group specific queries after a leave, saw {len(gsq)}")
        if not any(e.get("ev") == "leave" and e.get("role") == "querier" for e in events(binary)):
            problems.append("1: no leave event with role querier")
        r.kill()
        p.kill()
        time.sleep(1)

        # --- 2. upstream + querier: nothing is forwarded or joined
        p = start_proxy(tmp, "q2", base + ["phyint u0 upstream", "  altnet 10.0.1.0/24",
                                           "phyint br0 querier", "phyint lo disabled"], binary)
        procs.append(p)
        snd = Proc(f"nsenter --net=/run/netns/src python3 -c '{SENDER}' q-sender", os.devnull)
        procs.append(snd)
        t0 = time.time()
        r = Proc(f"nsenter --net=/run/netns/rx1 python3 -u -c '{RECV}' q-recv", os.path.join(tmp, "r2.log"))
        procs.append(r)
        time.sleep(4)
        data = 0
        with open(cap) as f:
            for line in f:
                if f"> {GROUP}.5000" in line and line[:1].isdigit() and float(line.split()[0]) >= t0:
                    data += 1
        joins = 0
        with open(upcap) as f:
            for line in f:
                if line[:1].isdigit() and float(line.split()[0]) >= t0 and "report" in line:
                    joins += 1
        print(f"2: {data} stream packets on the querier network, {joins} upstream joins")
        if data or joins:
            problems.append(f"2: querier interface got {data} packets / {joins} upstream joins, expected none")
        r.kill()
        snd.kill()
        p.kill()
        time.sleep(1)

        # --- 3. election on
        p = start_proxy(tmp, "q3", base + ["querierelection on", "phyint br0 querier",
                                           "phyint u0 disabled", "phyint lo disabled"], binary)
        procs.append(p)
        hi = Proc(f"nsenter --net=/run/netns/rx3 python3 -c '{FAKE_QUERIER}' 10.0.2.200", os.devnull)
        procs.append(hi)
        t0 = time.time()
        time.sleep(6.5)
        n = len(queries(cap, ME, t0))
        print(f"3: higher querier present, we sent {n} general queries in 6.5 s")
        if n < 2:
            problems.append(f"3: a querier with a higher address silenced us ({n} queries)")
        hi.kill()
        lo = Proc(f"nsenter --net=/run/netns/rx2 python3 -c '{FAKE_QUERIER}' 10.0.2.12", os.devnull)
        procs.append(lo)
        time.sleep(1)
        t1 = time.time()
        time.sleep(8)
        n = len(queries(cap, ME, t1))
        st = json.loads(ns("rtr", f"{binary} -S {SOCK}"))
        br = next((i for i in st["interfaces"] if i["name"] == "br0"), {})
        print(f"3: lower querier present, we sent {n} general queries in 8 s, status querier={br.get('querier')}")
        if n:
            problems.append(f"3: we kept querying next to a lower querier ({n} queries)")
        if br.get("querier") != "10.0.2.12":
            problems.append(f"3: status shows querier {br.get('querier')}, expected 10.0.2.12")
        lo.kill()
        t2 = time.time()
        time.sleep(9)
        back = queries(cap, ME, t2)
        print(f"3: lower querier gone, our first query after {round(back[0] - t2, 1) if back else None} s")
        if not back or back[0] - t2 > 8.5:
            problems.append("3: we did not resume querying after the other querier stopped")
        ev = [e.get("ev") for e in events(binary)]
        if "querier_lost" not in ev or "querier_resumed" not in ev:
            problems.append(f"3: events {sorted(set(ev))}, expected querier_lost and querier_resumed")
        p.kill()
        time.sleep(1)

        # --- 4. election off (default): keep querying
        p = start_proxy(tmp, "q4", base + ["phyint br0 querier", "phyint u0 disabled", "phyint lo disabled"], binary)
        procs.append(p)
        lo = Proc(f"nsenter --net=/run/netns/rx2 python3 -c '{FAKE_QUERIER}' 10.0.2.12", os.devnull)
        procs.append(lo)
        t3 = time.time()
        time.sleep(8)
        n = len(queries(cap, ME, t3))
        print(f"4: election off, lower querier present, we sent {n} general queries in 8 s")
        if n < 2:
            problems.append(f"4: with election off we stopped querying ({n} queries)")
        if any(e.get("ev") == "querier_lost" for e in events(binary)):
            problems.append("4: querier_lost with election off")
        lo.kill()
        p.kill()
    finally:
        for pr in procs:
            pr.kill()
        time.sleep(0.3)
        teardown()

    for pr in problems:
        print("FAIL:", pr)
    print("querier test passed" if not problems else f"{len(problems)} problems")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
IGMPv1/v2/v3 compatibility matrix for igmpproxy-plus (Linux, root).

Uses the namespaces from nstest.py: sender -> proxy -> two receivers on one
LAN segment. Each receiver is forced to an IGMP version with
net.ipv4.conf.<if>.force_igmp_version, like an old set-top box would be.

For every query version (3, 2) and fast leave mode (off, on, auto) it checks:
  - every host version receives the stream
  - after a leave the stream stops (IGMPv1 has no leave message: it ages out)
  - in mixed v2+v3 setups, the host that stays keeps its stream

usage: compat-test.py <igmpproxy binary>   (prints a table, exit 1 on failure)
"""
import json
import os
import signal
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nstest import GROUP, PORT, RECV, SENDER, Proc, ns, pkt_times, setup, teardown  # noqa: E402

SOCK = "/var/run/igmpproxy.sock"


def scenario(binary, qv, mode, hosts, leaver):
    """hosts: {1: ver, 2: ver}; leaver: receiver number that leaves (or None)."""
    tmp = tempfile.mkdtemp(prefix="compat-")
    conf = os.path.join(tmp, "igmpproxy.conf")
    with open(conf, "w") as f:
        f.write(f"queryversion {qv}\nfastleave {mode}\n"
                "phyint u0 upstream\n  altnet 10.0.1.0/24\n"
                "phyint br0 downstream\nphyint lo disabled\n")
    setup()
    procs = []
    res = {}
    try:
        dumps = {}
        for i in hosts:
            ns(f"rx{i}", f"sysctl -qw net.ipv4.conf.r{i}.force_igmp_version={hosts[i]}")
            dumps[i] = os.path.join(tmp, f"rx{i}.txt")
            procs.append(Proc(f"nsenter --net=/run/netns/rx{i} tcpdump -tt -n -l -i r{i} udp port {PORT}", dumps[i]))
        procs.append(Proc(f"nsenter --net=/run/netns/rtr {binary} -d -v -v {conf}", os.path.join(tmp, "proxy.log")))
        time.sleep(1)
        procs.append(Proc(f"nsenter --net=/run/netns/src python3 -c '{SENDER}' compat-sender", os.devnull))
        time.sleep(0.5)
        rx = {}
        for i in hosts:
            rx[i] = Proc(f"nsenter --net=/run/netns/rx{i} python3 -u -c '{RECV}' compat-recv", os.path.join(tmp, f"r{i}.log"))
            procs.append(rx[i])
            time.sleep(1.5)       # staggered, so v2 report suppression cannot hide anyone
        time.sleep(3)

        st = json.loads(ns("rtr", f"{binary} -S {SOCK}"))
        g = next((g for g in st["groups"] if g["group"] == GROUP), None)
        res["compat"] = g["members"][0]["compat"] if g and g["members"] else None

        t_leave = time.time()
        for i in hosts:
            got = [t for t in pkt_times(dumps[i]) if t_leave - 2 < t <= t_leave]
            res[f"rx{i}_receives"] = len(got) > 20
        if leaver:
            os.killpg(rx[leaver].p.pid, signal.SIGUSR1)
        time.sleep(5)
        if leaver:
            after = [t for t in pkt_times(dumps[leaver]) if t > t_leave]
            res["stop_s"] = round(after[-1] - t_leave, 2) if after else 0.0
            for i in hosts:
                if i != leaver:
                    ts = [t for t in pkt_times(dumps[i]) if t > t_leave - 1]
                    gaps = [b - a for a, b in zip(ts, ts[1:])]
                    res["stayer_gap_s"] = round(max(gaps), 2) if gaps else None
    finally:
        for p in procs:
            p.kill()
        time.sleep(0.3)
        teardown()
    return res


CASES = [
    # name,                hosts,          leaver
    ("v1 host",            {1: 1},         None),
    ("v2 host",            {1: 2},         1),
    ("v3 host",            {1: 3},         1),
    ("v2+v3, v3 leaves",   {1: 2, 2: 3},   2),
    ("v2+v3, v2 leaves",   {1: 2, 2: 3},   1),
    ("v2+v2, one leaves",  {1: 2, 2: 2},   2),
]


def expect(qv, mode, name, hosts, leaver, r):
    problems = []
    for i in hosts:
        if not r.get(f"rx{i}_receives"):
            problems.append(f"rx{i} (IGMPv{hosts[i]}) got no stream")
    if leaver:
        if r.get("stop_s") is None or r["stop_s"] > 3.0:
            if len(hosts) == 1:
                problems.append(f"stream did not stop after leave ({r.get('stop_s')} s)")
        if len(hosts) > 1:
            limit = 1.2 if mode == "on" else 0.2
            if r.get("stayer_gap_s") is None or r["stayer_gap_s"] > limit:
                problems.append(f"staying host lost the stream (gap {r.get('stayer_gap_s')} s)")
    return problems


def main():
    binary = sys.argv[1]
    failed = 0
    print(f"{'query':6} {'mode':5} {'case':20} {'compat':7} {'stop':>6} {'stayer gap':>10}  result")
    for qv in (3, 2):
        for mode in ("off", "on", "auto"):
            for name, hosts, leaver in CASES:
                r = scenario(binary, qv, mode, hosts, leaver)
                probs = expect(qv, mode, name, hosts, leaver, r)
                failed += bool(probs)
                print(f"v{qv:<5} {mode:5} {name:20} {str(r.get('compat')):7} "
                      f"{str(r.get('stop_s', '-')):>6} {str(r.get('stayer_gap_s', '-')):>10}  "
                      f"{'ok' if not probs else 'FAIL: ' + '; '.join(probs)}", flush=True)
    print("\nall compatibility cases passed" if not failed else f"\n{failed} cases failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()

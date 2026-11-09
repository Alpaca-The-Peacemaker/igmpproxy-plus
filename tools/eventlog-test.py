#!/usr/bin/env python3
"""
Event log and refresh test for igmpproxy-plus (Linux, root).

Uses the namespaces from nstest.py: two IGMPv2 receivers join a group, one
leaves, "igmpproxy -Q" asks for reports, then "igmpproxy -E" must return
valid JSON Lines with the expected events. A tiny eventlogsize checks the
rotation (current file + .1, never more).

usage: eventlog-test.py <igmpproxy binary>   (exit 1 on failure)
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


def main():
    binary = sys.argv[1]
    tmp = tempfile.mkdtemp(prefix="evlog-")
    log = os.path.join(tmp, "events.jsonl")
    conf = os.path.join(tmp, "igmpproxy.conf")
    with open(conf, "w") as f:
        f.write(f"queryversion 3\nfastleave off\neventlog {log}\n"
                "phyint u0 upstream\n  altnet 10.0.1.0/24\n"
                "phyint br0 downstream\nphyint lo disabled\n")
    setup()
    procs, problems = [], []
    try:
        for i in (1, 2):
            ns(f"rx{i}", f"sysctl -qw net.ipv4.conf.r{i}.force_igmp_version=2")
        procs.append(Proc(f"nsenter --net=/run/netns/rtr {binary} -d -v {conf}", os.path.join(tmp, "proxy.log")))
        time.sleep(1)
        procs.append(Proc(f"nsenter --net=/run/netns/src python3 -c '{SENDER}' ev-sender", os.devnull))
        rx = {}
        for i in (1, 2):      # rx2 joins last, so in IGMPv2 its leave is sent
            rx[i] = Proc(f"nsenter --net=/run/netns/rx{i} python3 -u -c '{RECV}' ev-recv", os.path.join(tmp, f"r{i}.log"))
            procs.append(rx[i])
            time.sleep(1.5)
        os.killpg(rx[2].p.pid, signal.SIGUSR1)
        time.sleep(1)

        q = ns("rtr", f"{binary} -Q {SOCK}")
        if '"ok":true' not in q:
            problems.append(f"-Q answered {q!r}")
        time.sleep(11)        # members answer within the response interval

        out = ns("rtr", f"{binary} -E {SOCK}")
        events = []
        for n, line in enumerate(out.splitlines(), 1):
            try:
                events.append(json.loads(line))
            except ValueError:
                problems.append(f"line {n} is not JSON: {line[:120]!r}")
        kinds = [e.get("ev") for e in events]
        print(f"{len(events)} events: " + ", ".join(sorted(set(kinds))))

        def has(ev, **kw):
            return any(e.get("ev") == ev and all(e.get(k) == v for k, v in kw.items()) for e in events)

        for ev, kw in [
            ("join", {"group": GROUP, "host": "10.0.2.11"}),
            ("join", {"group": GROUP, "host": "10.0.2.12"}),
            ("leave", {"group": GROUP, "host": "10.0.2.12", "action": "keeps", "others": 1}),
            ("query", {"kind": "general", "why": "requested"}),
            ("forward_start", {"group": GROUP}),
            ("upstream_join", {"group": GROUP}),
            ("report", {"group": GROUP, "host": "10.0.2.11"}),
        ]:
            if not has(ev, **kw):
                problems.append(f"missing {ev} {kw}")
        if not all("ts" in e and e["ts"].endswith("Z") for e in events):
            problems.append("event without a UTC timestamp")

        # rotation: second proxy with a 1 KiB limit, 40 refresh queries
        procs[0].kill()
        time.sleep(0.5)
        log2 = os.path.join(tmp, "rot.jsonl")
        conf2 = os.path.join(tmp, "rot.conf")
        with open(conf2, "w") as f:
            f.write(f"eventlog {log2}\neventlogsize 1\n"
                    "phyint u0 upstream\n  altnet 10.0.1.0/24\n"
                    "phyint br0 downstream\nphyint lo disabled\n")
        procs.append(Proc(f"nsenter --net=/run/netns/rtr {binary} -d {conf2}", os.path.join(tmp, "proxy2.log")))
        time.sleep(1)
        for _ in range(40):
            ns("rtr", f"{binary} -Q {SOCK}")
        size = os.path.getsize(log2)
        if size > 1024 + 400:
            problems.append(f"current log is {size} bytes, rotation did not happen")
        if not os.path.exists(log2 + ".1"):
            problems.append("no rotated file (.1)")
        if os.path.exists(log2 + ".2"):
            problems.append("more than one rotated file")
        dump = ns("rtr", f"{binary} -E {SOCK}").splitlines()
        if not dump or any(not l.startswith("{") or not l.endswith("}") for l in dump):
            problems.append("rotated dump has broken lines")

        # status still works through the command protocol
        st = json.loads(ns("rtr", f"{binary} -S {SOCK}"))
        if "groups" not in st:
            problems.append("-S did not return the status")
    finally:
        for p in procs:
            p.kill()
        time.sleep(0.3)
        teardown()

    for p in problems:
        print("FAIL:", p)
    print("event log test passed" if not problems else f"{len(problems)} problems")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()

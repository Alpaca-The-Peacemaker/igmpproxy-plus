#!/usr/bin/env python3
"""
igmpproxy-plus network namespace test.

Topology (all inside one Linux host / container, run as root):

  [ns src] s0 10.0.1.2 --- u0 10.0.1.1 [ns rtr] br0 10.0.2.1 --+-- r1 10.0.2.11 [ns rx1]
                               (upstream)      (downstream)    +-- r2 10.0.2.12 [ns rx2]

A sender in "src" sends UDP to 239.1.1.1 every 20 ms. Receivers join and
leave with real sockets. tcpdump on each receiver's interface records when
multicast packets actually arrive, so we measure what the proxy forwards.

usage: nstest.py <igmpproxy binary> <label> [extra global config lines...]
"""
import json
import os
import signal
import subprocess
import sys
import tempfile
import time

GROUP = "239.1.1.1"
PORT = 5000


def sh(cmd, check=True):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(f"{cmd}\n{r.stdout}{r.stderr}")
    return r.stdout


def ns(n, cmd, check=True):
    return sh(f"nsenter --net=/run/netns/{n} {cmd}", check)


def teardown():
    for n in ("src", "rtr", "rx1", "rx2", "rx3"):
        sh(f"ip netns del {n}", check=False)
    sh("pkill -f 'nstest-(sender|recv)'", check=False)


def setup():
    teardown()
    for n in ("src", "rtr", "rx1", "rx2", "rx3"):
        sh(f"ip netns add {n}")
        ns(n, "ip link set lo up")
    sh("ip link add s0 netns src type veth peer name u0 netns rtr")
    ns("src", "ip addr add 10.0.1.2/24 dev s0")
    ns("src", "ip link set s0 up")
    ns("src", "ip route add default via 10.0.1.1")
    ns("rtr", "ip addr add 10.0.1.1/24 dev u0")
    ns("rtr", "ip link set u0 up")
    ns("rtr", "ip link add br0 type bridge mcast_snooping 0")
    ns("rtr", "ip addr add 10.0.2.1/24 dev br0")
    ns("rtr", "ip link set br0 up")
    for i in (1, 2, 3):
        sh(f"ip link add b{i} netns rtr type veth peer name r{i} netns rx{i}")
        ns("rtr", f"ip link set b{i} master br0")
        ns("rtr", f"ip link set b{i} up")
        ns(f"rx{i}", f"ip addr add 10.0.2.1{i}/24 dev r{i}")
        ns(f"rx{i}", f"ip link set r{i} up")
        ns(f"rx{i}", "ip route add default via 10.0.2.1")
    for n in ("rtr",):
        ns(n, "sysctl -qw net.ipv4.conf.all.rp_filter=0")
        ns(n, "sysctl -qw net.ipv4.conf.u0.rp_filter=0")


SENDER = f"""
import socket, time, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 8)
i = 0
while True:
    s.sendto(str(i).encode(), ("{GROUP}", {PORT})); i += 1
    time.sleep(0.02)
"""

# Receiver: join, keep the socket open, leave on SIGUSR1 (prints leave time),
# rejoin on SIGUSR2.
RECV = f"""
import socket, struct, time, signal, sys
mreq = struct.pack("4s4s", socket.inet_aton("{GROUP}"), socket.inet_aton("0.0.0.0"))
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("", {PORT}))
def join():
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    print("JOIN %.3f" % time.time(), flush=True)
def leave(*a):
    s.setsockopt(socket.IPPROTO_IP, socket.IP_DROP_MEMBERSHIP, mreq)
    print("LEAVE %.3f" % time.time(), flush=True)
signal.signal(signal.SIGUSR1, leave)
signal.signal(signal.SIGUSR2, lambda *a: join())
join()
s.settimeout(0.2)
while True:
    try: s.recv(100)
    except Exception: pass
"""


class Proc:
    def __init__(self, cmd, out):
        self.f = open(out, "w")
        self.p = subprocess.Popen(cmd, shell=True, stdout=self.f, stderr=subprocess.STDOUT,
                                  preexec_fn=os.setsid)

    def kill(self, sig=signal.SIGTERM):
        try:
            os.killpg(self.p.pid, sig)
        except ProcessLookupError:
            pass

    def signal(self, sig):
        self.p.send_signal(sig)


def pkt_times(pcap_txt):
    """Arrival times of multicast data packets from a tcpdump -tt text log."""
    ts = []
    with open(pcap_txt) as f:
        for line in f:
            if f"> {GROUP}.{PORT}" in line:
                ts.append(float(line.split()[0]))
    return ts


def run(binary, label, extra):
    tmp = tempfile.mkdtemp(prefix="nstest-")
    conf = os.path.join(tmp, "igmpproxy.conf")
    with open(conf, "w") as f:
        for l in extra:
            f.write(l + "\n")
        f.write("phyint u0 upstream\n  altnet 10.0.1.0/24\n")
        f.write("phyint br0 downstream\n")
        # optional per-interface lines, e.g. NSTEST_DOWNSTREAM="blacklist 239.1.1.1/32"
        for l in filter(None, os.environ.get("NSTEST_DOWNSTREAM", "").split(";")):
            f.write("  " + l.strip() + "\n")
        f.write("phyint lo disabled\n")
    results = {"label": label, "config": extra}

    setup()
    procs = []
    try:
        dumps = {}
        for i in (1, 2):
            out = os.path.join(tmp, f"rx{i}.txt")
            procs.append(Proc(f"nsenter --net=/run/netns/rx{i} tcpdump -tt -n -l -i r{i} udp port {PORT}", out))
            dumps[i] = out
        upd = os.path.join(tmp, "up.txt")
        procs.append(Proc("nsenter --net=/run/netns/rtr tcpdump -tt -n -l -i u0 igmp", upd))
        downd = os.path.join(tmp, "down.txt")
        procs.append(Proc("nsenter --net=/run/netns/rtr tcpdump -tt -n -l -v -i br0 igmp", downd))
        sock_supported = "plus" in label
        prox_log = os.path.join(tmp, "proxy.log")
        proxy = Proc(f"nsenter --net=/run/netns/rtr {binary} -d -v -v {conf}", prox_log)
        procs.append(proxy)
        time.sleep(1.0)
        procs.append(Proc(f"nsenter --net=/run/netns/src python3 -c '{SENDER}' nstest-sender", os.path.join(tmp, "snd.txt")))
        time.sleep(0.5)

        # --- scenario A: single receiver joins then leaves
        r1log = os.path.join(tmp, "r1.log")
        r1 = Proc(f"nsenter --net=/run/netns/rx1 python3 -u -c '{RECV}' nstest-recv", r1log)
        procs.append(r1)
        time.sleep(4)
        if sock_supported:
            results["status_while_joined"] = json.loads(ns("rtr", f"{binary} -S /var/run/igmpproxy.sock"))
        t_leave = time.time()
        os.killpg(r1.p.pid, signal.SIGUSR1)
        time.sleep(12)
        ts = [t for t in pkt_times(dumps[1]) if t > t_leave - 5]
        got = [t for t in ts if t <= t_leave]
        after = [t for t in ts if t > t_leave]
        results["A_single_leave"] = {
            "received_before_leave": len(got) > 0,
            "stop_latency_s": round(after[-1] - t_leave, 3) if after else 0.0,
            "packets_after_leave": len(after),
        }
        r1.kill()
        time.sleep(1)

        # --- scenario B: two receivers, one leaves, the other must keep the stream.
        # With IGMPv2 queries (queryversion 2) hosts run in IGMPv2 mode with report
        # suppression: a host that heard another host's report stays silent and
        # sends no leave. Stagger the joins so rx1 reports last and does leave.
        r2 = Proc(f"nsenter --net=/run/netns/rx2 python3 -u -c '{RECV}' nstest-recv", os.path.join(tmp, "r2b.log"))
        time.sleep(1.5)
        r1 = Proc(f"nsenter --net=/run/netns/rx1 python3 -u -c '{RECV}' nstest-recv", os.path.join(tmp, "r1b.log"))
        procs += [r1, r2]
        time.sleep(3)
        t_leave = time.time()
        os.killpg(r1.p.pid, signal.SIGUSR1)
        time.sleep(6)
        ts = [t for t in pkt_times(dumps[2]) if t > t_leave - 2]
        gaps = [b - a for a, b in zip(ts, ts[1:])]
        results["B_other_receiver_keeps_stream"] = {
            "packets_after_leave": len([t for t in ts if t > t_leave]),
            "max_gap_s": round(max(gaps), 3) if gaps else None,
        }
        if sock_supported:
            st = json.loads(ns("rtr", f"{binary} -S /var/run/igmpproxy.sock"))
            results["status_after_B"] = st
        r1.kill(); r2.kill()
        os.killpg(r2.p.pid, signal.SIGUSR1)
        time.sleep(8)

        if sock_supported:
            def group_state():
                st = json.loads(ns("rtr", f"{binary} -S /var/run/igmpproxy.sock"))
                for g in st["groups"]:
                    if g["group"] == GROUP:
                        m = g["members"][0] if g["members"] else {}
                        return {"leaves": g["leaves"], "forwarding": bool(m.get("forwarding")),
                                "hosts": sorted(h["address"] for h in m.get("hosts", [])),
                                "compat": m.get("compat")}
                return {"leaves": 0, "forwarding": False, "hosts": [], "compat": None}

            # --- scenario C: both receivers join at the same moment, rx1 leaves.
            # With IGMPv3 queries there is no report suppression: both must be
            # tracked and rx1's leave must arrive.
            r1 = Proc(f"nsenter --net=/run/netns/rx1 python3 -u -c '{RECV}' nstest-recv", os.path.join(tmp, "r1c.log"))
            r2 = Proc(f"nsenter --net=/run/netns/rx2 python3 -u -c '{RECV}' nstest-recv", os.path.join(tmp, "r2c.log"))
            procs += [r1, r2]
            time.sleep(4)
            before = group_state()
            t_leave = time.time()
            os.killpg(r1.p.pid, signal.SIGUSR1)
            time.sleep(4)
            after = group_state()
            ts = [t for t in pkt_times(dumps[2]) if t > t_leave - 2]
            gaps = [b - a for a, b in zip(ts, ts[1:])]
            results["C_simultaneous_joins"] = {
                "hosts_before_leave": before["hosts"],
                "leave_counted": after["leaves"] > before["leaves"],
                "hosts_after_leave": after["hosts"],
                "rx2_max_gap_s": round(max(gaps), 3) if gaps else None,
            }
            r1.kill(); r2.kill()
            os.killpg(r2.p.pid, signal.SIGUSR1)
            time.sleep(6)

            # --- scenario D: an IGMPv2-only host on an IGMPv3 interface.
            ns("rx2", "sysctl -qw net.ipv4.conf.r2.force_igmp_version=2")
            r2 = Proc(f"nsenter --net=/run/netns/rx2 python3 -u -c '{RECV}' nstest-recv", os.path.join(tmp, "r2d.log"))
            procs.append(r2)
            time.sleep(4)
            joined = group_state()
            t_leave = time.time()
            os.killpg(r2.p.pid, signal.SIGUSR1)
            time.sleep(6)
            ts = [t for t in pkt_times(dumps[2]) if t > t_leave - 4]
            got = [t for t in ts if t <= t_leave]
            after = [t for t in ts if t > t_leave]
            results["D_igmpv2_host"] = {
                "received": len(got) > 0,
                "compat_while_joined": joined["compat"],
                "stop_latency_s": round(after[-1] - t_leave, 3) if after else 0.0,
            }
            r2.kill()
            ns("rx2", "sysctl -qw net.ipv4.conf.r2.force_igmp_version=0")

        with open(downd) as f:
            q = [l for l in f if "query" in l]
        general = [l for l in q if "gaddr" not in l]
        results["queries_on_lan"] = {
            "v3": sum("query v3" in l for l in q),
            "v2": sum("query v2" in l for l in q),
            # periodic general queries; group specific ones may be v2 on purpose
            "general_v3": sum("query v3" in l for l in general),
            "general_v2": sum("query v2" in l for l in general),
        }
        with open(upd) as f:
            results["upstream_igmp"] = [l.strip() for l in f if "igmp" in l.lower()][-6:]
    finally:
        for p in procs:
            p.kill()
        time.sleep(0.3)
        results["proxy_log_tail"] = open(prox_log).read().splitlines()[-15:] if os.path.exists(prox_log) else []
        teardown()
    return results


if __name__ == "__main__":
    print(json.dumps(run(sys.argv[1], sys.argv[2], sys.argv[3:]), indent=1))

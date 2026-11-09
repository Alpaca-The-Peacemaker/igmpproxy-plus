#!/usr/bin/env python3
"""
Scale test for igmpproxy-plus (Linux, root): many streams, many receivers.

Topology (network namespaces, as nstest.py, plus one "hosts" namespace):
  [src] sends to N groups --- u0 [rtr: igmpproxy] br0 --- [hosts] 100 addresses

The receivers are simulated: IGMPv2 reports and leaves are written with a
raw socket from 100 different source addresses, so 100 hosts cost one
namespace. Each host joins 5 groups of its own plus 5 shared ones, so about
1000 memberships over N groups, some groups with one host, many with several.

Phases and what is measured:
  1. join      all memberships at once: time until every group is forwarded,
               kernel routes (ip mroute), upstream memberships (/proc/net/igmp)
  2. steady    reports repeated every 10 s for a while: daemon CPU, memory,
               status (-S) latency and size, event log growth
  3. leave     single-host groups leave while everything else keeps running:
               leave-to-stop time from the event log (expected about 2 s with
               2 x 1000 ms checks), and no other group may be affected

usage: scale-test.py <igmpproxy binary> [groups] [hosts] [steady_s]
"""
import json
import os
import random
import socket
import struct
import sys
import tempfile
import threading
import time
from datetime import datetime

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nstest import Proc, ns, setup, sh, teardown  # noqa: E402

SOCK = "/var/run/igmpproxy.sock"
PORT = 5000


def group_ip(i):
    return "239.2.%d.%d" % (i // 250, i % 250 + 1)


def host_ip(i):
    return "10.0.2.%d" % (100 + i)


def csum(b):
    if len(b) % 2:
        b += b"\0"
    s = sum(struct.unpack("!%dH" % (len(b) // 2), b))
    s = (s >> 16) + (s & 0xFFFF)
    s += s >> 16
    return ~s & 0xFFFF


def igmp_packet(src, group, leave=False):
    """IPv4 header with Router Alert + IGMPv2 report (0x16) or leave (0x17)."""
    dst = "224.0.0.2" if leave else group
    igmp = struct.pack("!BBH4s", 0x17 if leave else 0x16, 0, 0, socket.inet_aton(group))
    igmp = igmp[:2] + struct.pack("!H", csum(igmp)) + igmp[4:]
    opt = b"\x94\x04\x00\x00"
    ihl = 6
    hdr = struct.pack("!BBHHHBBH4s4s", (4 << 4) | ihl, 0xC0, ihl * 4 + len(igmp), 0, 0, 1, 2, 0,
                      socket.inet_aton(src), socket.inet_aton(dst)) + opt
    hdr = hdr[:10] + struct.pack("!H", csum(hdr)) + hdr[12:]
    return hdr + igmp, dst


# Runs inside the "hosts" namespace: reads "R src group" / "L src group"
# lines on stdin and sends the packets.
SENDER_IGMP = r'''
import socket, struct, sys
def csum(b):
    if len(b) % 2: b += b"\0"
    s = sum(struct.unpack("!%dH" % (len(b)//2), b)); s = (s >> 16) + (s & 0xFFFF); s += s >> 16
    return ~s & 0xFFFF
s = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_RAW)
s.setsockopt(socket.IPPROTO_IP, socket.IP_HDRINCL, 1)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, b"hh")
for line in sys.stdin:
    k, src, group = line.split()
    leave = k == "L"
    dst = "224.0.0.2" if leave else group
    igmp = struct.pack("!BBH4s", 0x17 if leave else 0x16, 0, 0, socket.inet_aton(group))
    igmp = igmp[:2] + struct.pack("!H", csum(igmp)) + igmp[4:]
    hdr = struct.pack("!BBHHHBBH4s4s", 0x46, 0xC0, 24 + len(igmp), 0, 0, 1, 2, 0,
                      socket.inet_aton(src), socket.inet_aton(dst)) + b"\x94\x04\x00\x00"
    hdr = hdr[:10] + struct.pack("!H", csum(hdr)) + hdr[12:]
    s.sendto(hdr + igmp, (dst, 0))
    sys.stdout.write("ok\n"); sys.stdout.flush()
'''

STREAMS = r'''
import socket, sys, time
n, pps = int(sys.argv[1]), float(sys.argv[2])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 8)
groups = ["239.2.%d.%d" % (i // 250, i % 250 + 1) for i in range(n)]
payload = b"x" * 200
gap = 1.0 / pps
while True:
    t = time.time()
    for g in groups:
        s.sendto(payload, (g, 5000))
    d = gap - (time.time() - t)
    if d > 0: time.sleep(d)
'''


class Igmp:
    def __init__(self):
        import subprocess
        self.p = subprocess.Popen(["nsenter", "--net=/run/netns/hosts", "python3", "-c", SENDER_IGMP],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.lock = threading.Lock()

    def send(self, kind, src, group):
        with self.lock:
            self.p.stdin.write("%s %s %s\n" % (kind, src, group))
            self.p.stdout.readline()

    def close(self):
        self.p.kill()


def proc_stat(pid):
    with open("/proc/%d/stat" % pid) as f:
        v = f.read().rsplit(")", 1)[1].split()
    ticks = int(v[11]) + int(v[12])
    with open("/proc/%d/status" % pid) as f:
        rss = next(int(l.split()[1]) for l in f if l.startswith("VmRSS"))
    return ticks / os.sysconf("SC_CLK_TCK"), rss


def status(binary):
    t = time.time()
    out = ns("rtr", "%s -S %s" % (binary, SOCK))
    return json.loads(out), len(out), time.time() - t


def main():
    binary = sys.argv[1]
    NG = int(sys.argv[2]) if len(sys.argv) > 2 else 500
    NH = int(sys.argv[3]) if len(sys.argv) > 3 else 100
    STEADY = int(sys.argv[4]) if len(sys.argv) > 4 else 60
    random.seed(1)
    tmp = tempfile.mkdtemp(prefix="scale-")
    log = os.path.join(tmp, "events.jsonl")
    conf = os.path.join(tmp, "igmpproxy.conf")
    with open(conf, "w") as f:
        f.write("queryversion 2\nfastleave off\nlastmemberinterval 1000ms\nlastmembercount 2\n"
                "queryinterval 20s\nqueryresponseinterval 5s\nhostlimit 256\n"
                f"eventlog {log}\neventlogsize 65536\neventlogreports off\n"
                + os.environ.get("SCALE_EXTRA", "").replace(";", "\n") +
                "phyint u0 upstream\n  altnet 10.0.1.0/24\n"
                "phyint br0 downstream\nphyint lo disabled\n")

    # memberships: 5 own groups per host (every group gets one), 5 shared
    members = {}
    for h in range(NH):
        own = [(h * 5 + k) % NG for k in range(5)]
        extra = random.sample(range(NG), 5)
        members[h] = sorted(set(own + extra))
    hosts_of = {g: [h for h in members if g in members[h]] for g in range(NG)}
    single = [g for g in range(NG) if len(hosts_of[g]) == 1]
    pairs = [(h, g) for h in members for g in members[h]]
    print("setup: %d groups, %d hosts, %d memberships, %d single-host groups"
          % (NG, NH, len(pairs), len(single)), flush=True)

    setup()
    procs, res, problems = [], {}, []
    igmp = None
    try:
        sh("ip netns add hosts")
        ns("hosts", "ip link set lo up")
        sh("ip link add bh netns rtr type veth peer name hh netns hosts")
        ns("rtr", "ip link set bh master br0")
        ns("rtr", "ip link set bh up")
        for h in range(NH):
            ns("hosts", "ip addr add %s/24 dev hh" % host_ip(h))
        ns("hosts", "ip link set hh up")
        res["igmp_max_memberships"] = ns("rtr", "sysctl -n net.ipv4.igmp_max_memberships").strip()

        procs.append(Proc(f"nsenter --net=/run/netns/rtr {binary} -d -v {conf}", os.path.join(tmp, "proxy.log")))
        time.sleep(1.5)
        pid = int(sh(f"pgrep -n -f '^{binary} -d -v {conf}'").split()[0])
        procs.append(Proc(f"nsenter --net=/run/netns/src python3 -c '{STREAMS}' {NG} 5", os.devnull))
        igmp = Igmp()
        time.sleep(1)
        cpu0, _ = proc_stat(pid)

        # --- 1. join -----------------------------------------------------
        t0 = time.time()
        for h, g in pairs:
            igmp.send("R", host_ip(h), group_ip(g))
        t_sent = time.time() - t0
        full = None
        while time.time() - t0 < 60:
            st, _, _ = status(binary)
            fwd = [g for g in st["groups"] if g["group"].startswith("239.2.")
                   and any(m["forwarding"] for m in g["members"]) and g.get("bytes")]
            if len(fwd) >= NG:
                full = time.time() - t0
                break
            time.sleep(0.25)
        res["join_reports_sent_s"] = round(t_sent, 2)
        res["join_all_forwarding_s"] = round(full, 2) if full else None
        res["join_groups_forwarding"] = len(fwd)
        # drops on the proxy's raw IGMP socket (last column of /proc/net/raw, protocol 2)
        drops = 0
        for line in ns("rtr", "cat /proc/net/raw").splitlines()[1:]:
            f = line.split()
            if len(f) > 12 and f[1].endswith(":0002"):
                drops += int(f[-1])
        res["igmp_socket_drops"] = drops
        if drops:
            problems.append("the kernel dropped %d IGMP messages for the proxy (receive buffer full)" % drops)
        mine = [g for g in st["groups"] if g["group"].startswith("239.2.")]
        res["join_diag"] = {
            "groups_in_table": len(mine),
            "with_forwarding_member": sum(any(m["forwarding"] for m in g["members"]) for g in mine),
            "with_byte_counter": sum("bytes" in g for g in mine),
            "with_bytes_gt_0": sum(bool(g.get("bytes")) for g in mine),
            "with_source": sum(bool(g.get("sources")) for g in mine),
        }
        if full is None:
            problems.append("only %d of %d groups forwarding after 60 s" % (len(fwd), NG))
        mroutes = ns("rtr", "ip mroute show").count("239.2.")
        res["kernel_routes"] = mroutes
        up = ns("rtr", "cat /proc/net/igmp")
        # count memberships listed under u0
        cnt, cur = 0, None
        for line in up.splitlines():
            if line and not line[0].isspace():
                cur = line.split()[1] if len(line.split()) > 1 else None
            elif cur == "u0" and line.split() and len(line.split()[0]) == 8:
                cnt += 1
        res["upstream_memberships_u0"] = cnt
        if cnt < NG:
            problems.append("only %d of %d groups joined upstream (kernel limit igmp_max_memberships=%s)"
                            % (cnt, NG, res["igmp_max_memberships"]))
        with open(os.path.join(tmp, "proxy.log")) as f:
            res["upstream_join_errors"] = f.read().count("can't join group")
        st, _, _ = status(binary)
        res["status_upstream_join_failures"] = st.get("upstream_join_failures")
        res["igmp_max_memberships_after_start"] = ns("rtr", "sysctl -n net.ipv4.igmp_max_memberships").strip()

        # --- 2. steady state ---------------------------------------------
        stop = threading.Event()
        left = set()

        def refresher():
            while not stop.wait(10):
                for h, g in pairs:
                    if (h, g) not in left:
                        igmp.send("R", host_ip(h), group_ip(g))
        th = threading.Thread(target=refresher, daemon=True)
        th.start()
        cpu1, _ = proc_stat(pid)
        lsize0 = os.path.getsize(log)
        t1 = time.time()
        lat, size = [], 0
        while time.time() - t1 < STEADY:
            _, size, l = status(binary)
            lat.append(l)
            time.sleep(2)
        cpu2, rss = proc_stat(pid)
        el = time.time() - t1
        res["steady_seconds"] = round(el)
        res["daemon_cpu_percent"] = round(100 * (cpu2 - cpu1) / el, 2)
        res["daemon_cpu_join_phase_s"] = round(cpu1 - cpu0, 2)
        res["daemon_rss_kib"] = rss
        res["status_json_kib"] = round(size / 1024, 1)
        res["status_latency_ms_avg"] = round(1000 * sum(lat) / len(lat))
        res["status_latency_ms_max"] = round(1000 * max(lat))
        res["eventlog_bytes_per_min_reports_off"] = round((os.path.getsize(log) - lsize0) * 60 / el)
        # a report line is ~120 bytes; one per membership per query cycle
        res["eventlog_estimate_mib_per_day_reports_on"] = round(
            len(pairs) * 120 * 86400 / 125 / 2**20, 1)

        # --- 3. leave under load ------------------------------------------
        sample = single[:50]
        sent = {}
        for g in sample:
            h = hosts_of[g][0]
            left.add((h, g))
            sent[group_ip(g)] = time.time()
            igmp.send("L", host_ip(h), group_ip(g))
            time.sleep(0.02)
        time.sleep(6)
        stops = {}
        with open(log) as f:
            for line in f:
                e = json.loads(line)
                if e.get("ev") == "forward_stop" and e.get("group") in sent:
                    t = datetime.fromisoformat(e["ts"].replace("Z", "+00:00")).timestamp()
                    stops.setdefault(e["group"], t - sent[e["group"]])
        d = sorted(stops.values())
        res["leave_stopped"] = "%d of %d" % (len(d), len(sample))
        if d:
            res["leave_to_stop_s_p50"] = round(d[len(d) // 2], 2)
            res["leave_to_stop_s_p95"] = round(d[int(len(d) * 0.95) - 1], 2)
            res["leave_to_stop_s_max"] = round(d[-1], 2)
        if len(d) < len(sample):
            problems.append("%d leaves did not stop their stream" % (len(sample) - len(d)))
        if d and d[-1] > 3.0:
            problems.append("slowest leave took %.2f s (expected about 2 s)" % d[-1])
        st, _, _ = status(binary)
        still = [g["group"] for g in st["groups"] if g["group"].startswith("239.2.")
                 and any(m["forwarding"] for m in g["members"])]
        res["groups_still_forwarding"] = len(still)
        if len(still) != NG - len(sample):
            problems.append("%d groups forwarding after the leaves, expected %d"
                            % (len(still), NG - len(sample)))
        stop.set()
    finally:
        if igmp:
            igmp.close()
        for p in procs:
            p.kill()
        time.sleep(0.3)
        sh("ip netns del hosts", check=False)
        teardown()

    print(json.dumps(res, indent=1))
    for p in problems:
        print("PROBLEM:", p)
    print("scale test passed" if not problems else "%d problems" % len(problems))
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()

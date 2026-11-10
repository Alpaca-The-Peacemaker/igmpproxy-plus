#!/bin/sh
#
# Forwarding test for igmpproxy-plus on FreeBSD / OPNsense (root).
#
# Two VNET jails on epair links play the provider (sender) and a receiver:
#
#   jail fbtsrc 10.99.1.2 --- 10.99.1.1 [proxy] 10.99.2.1 --- 10.99.2.11 jail fbtrx
#
# Checks: the stream reaches the receiver after its join, the start delay is
# reported, a leave stops the stream (fast leave auto, IGMPv3 host), and the
# event log has the expected events. Touches no existing interface.
#
# usage: freebsd-jail-test.sh [igmpproxy binary]     (exit 1 on failure)

BIN=${1:-/usr/local/sbin/igmpproxy-plus}
T=$(mktemp -d /tmp/fbt.XXXXXX)
SOCK=$T/proxy.sock
GROUP=239.1.1.1
FAIL=0

fail() { echo "FAIL: $*"; FAIL=1; }

cleanup() {
	[ -f "$T/proxy.pid" ] && kill "$(cat "$T/proxy.pid")" 2>/dev/null
	jail -r fbtsrc 2>/dev/null
	jail -r fbtrx 2>/dev/null
	sleep 1
	[ -n "$EA" ] && ifconfig "$EA" destroy 2>/dev/null
	[ -n "$EB" ] && ifconfig "$EB" destroy 2>/dev/null
	rm -rf "$T"
}
trap cleanup EXIT INT TERM

kldload -n if_epair 2>/dev/null
kldload -n ip_mroute 2>/dev/null

EA=$(ifconfig epair create) || { echo "cannot create an epair"; exit 1; }
EB=$(ifconfig epair create) || exit 1
PA=${EA%a}b
PB=${EB%a}b

jail -c name=fbtsrc vnet persist path=/ || { echo "VNET jails are not available"; exit 1; }
jail -c name=fbtrx vnet persist path=/ || exit 1
ifconfig "$PA" vnet fbtsrc
ifconfig "$PB" vnet fbtrx
ifconfig "$EA" inet 10.99.1.1/24 up
ifconfig "$EB" inet 10.99.2.1/24 up
jexec fbtsrc ifconfig "$PA" inet 10.99.1.2/24 up
jexec fbtsrc route -q add default 10.99.1.1
jexec fbtrx ifconfig "$PB" inet 10.99.2.11/24 up
jexec fbtrx route -q add default 10.99.2.1

cat > "$T/proxy.conf" <<EOF
controlsocket $SOCK
eventlog $T/events.jsonl
fastleave auto
queryversion 3
lastmemberinterval 1000ms
phyint $EA upstream
    altnet 10.99.1.0/24
phyint $EB downstream
EOF

cat > "$T/send.py" <<EOF
import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 8)
i = 0
while True:
    s.sendto(str(i).encode(), ("$GROUP", 5000)); i += 1
    time.sleep(0.02)
EOF

cat > "$T/recv.py" <<EOF
import socket, struct, signal, sys
mreq = struct.pack("4s4s", socket.inet_aton("$GROUP"), socket.inet_aton("10.99.2.11"))
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("", 5000))
s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
n = 0
def leave(*a):
    s.setsockopt(socket.IPPROTO_IP, socket.IP_DROP_MEMBERSHIP, mreq)
signal.signal(signal.SIGUSR1, leave)
out = open(sys.argv[1], "w")
while True:
    try:
        s.recv(2000)
    except InterruptedError:
        continue
    n += 1
    out.seek(0); out.write("%d\n" % n); out.flush()
EOF

count() { cat "$T/count" 2>/dev/null || echo 0; }

"$BIN" -t "$T/proxy.conf" > /dev/null || { echo "configuration rejected"; exit 1; }
"$BIN" -d -v "$T/proxy.conf" > "$T/proxy.log" 2>&1 &
echo $! > "$T/proxy.pid"
sleep 1

jexec fbtrx python3 "$T/recv.py" "$T/count" &
sleep 1.5
jexec fbtsrc python3 "$T/send.py" &
sleep 3

C1=$(count)
[ "$C1" -gt 50 ] || fail "receiver got $C1 packets in 3 s, expected about 150"
echo "received after join: $C1 packets"

ST=$("$BIN" -S "$SOCK")
echo "$ST" | grep -q "\"group\":\"$GROUP\",\"upstream\":\"joined\"" || fail "group not joined upstream: $ST"
DELAY=$(echo "$ST" | sed -n 's/.*"start_delay_ms":\([0-9-]*\).*/\1/p')
if [ -n "$DELAY" ]; then
	echo "start delay: $DELAY ms"
	{ [ "$DELAY" -ge 1000 ] && [ "$DELAY" -le 3500 ]; } || fail "start delay $DELAY ms, expected about 1500"
fi
echo "$ST" | grep -q '"bytes":[1-9]' || fail "no byte counters for the stream"

# leave: the only host, IGMPv3, fast leave auto -> the stream stops at once
pkill -USR1 -f "$T/recv.py"
sleep 1.5
C2=$(count)
sleep 2
C3=$(count)
echo "after leave: $C2 -> $C3 packets"
[ "$C3" -le $((C2 + 2)) ] || fail "stream still flowing after the leave ($C2 -> $C3)"

EV=$("$BIN" -E "$SOCK")
for e in join forward_start upstream_join leave forward_stop upstream_leave; do
	echo "$EV" | grep -q "\"ev\":\"$e\"" || fail "event $e missing"
done
echo "events: $(echo "$EV" | sed -n 's/.*"ev":"\([a-z_]*\)".*/\1/p' | sort -u | tr '\n' ' ')"

if [ $FAIL -ne 0 ]; then
	echo "--- proxy log"; tail -30 "$T/proxy.log"
	echo "FreeBSD jail test FAILED"
	exit 1
fi
echo "FreeBSD jail test passed"

# igmpproxy-plus

An alternative to [igmpproxy](https://github.com/pali/igmpproxy), forked from 0.4, with full
control over how fast multicast traffic stops after a receiver leaves, plus plugins for OpenWrt
(LuCI) and OPNsense.

> **Status:** the daemon passes the namespace tests below (`tools/nstest.py`). The OpenWrt
> packages are built with the 25.12.6 SDK and run on an x86/64 router with live IPTV traffic.
> The OPNsense plugin and FreeBSD port have not been built yet. IGMPv3 queries are planned.

### Measured (Linux namespaces, sender at 50 packets/s, `tools/nstest.py`)

| Configuration | Traffic after the only receiver leaves | Second receiver on the same segment when the first leaves |
|---|---|---|
| stock igmpproxy 0.4 | still flowing when the 12 s window ended (about 20 s expected) | no gap |
| stock + `quickleave` | still flowing when the 12 s window ended | no gap |
| plus, `fastleave off` (2 × 1 s) | **2.0 s** | no gap |
| plus, `fastleave off` (2 × 300 ms) | **0.6 s** | no gap |
| plus, `fastleave on` | **0 s** | ~1 s gap until it answers the group query |
| plus, `fastleave auto` | **0 s** | no gap (both hosts tracked) |

Note: igmpproxy sends IGMPv2 queries, so hosts run in IGMPv2 mode. In v2, a host that hears
another host's report for the same group stays quiet and later sends no leave. On a shared
segment without IGMP snooping, `auto` may therefore not know about every host. Use `off` with a
short interval there, or `on` if a short dip for other viewers is acceptable.

## Why stock igmpproxy leaves slowly

| Stock igmpproxy 0.4 | igmpproxy-plus |
|---|---|
| Last member query interval is hard-coded to **10 s**. With 2 queries, traffic keeps flowing for about 20 s after a leave. | Configurable from 100 ms to 25.5 s. Default **1 s** (the RFC 2236 value). |
| Timers tick in whole seconds. | Millisecond timers. |
| `quickleave` only sends the *upstream* leave early. Downstream forwarding continues until the group ages out. | `fastleave` stops *downstream* forwarding on that interface at once: `off`, `on` or `auto`, set globally or per interface. |
| A leave queries **every** downstream interface and ages the whole group. | Leaves are handled per interface. Other interfaces are not affected. |
| Host tracking uses a lossy hash (`hashtablesize`), so it can't tell when the last host is gone. | Exact per-host list per group and interface, each host with its own timer. |
| Robustness, query interval and response interval are fixed. | All configurable, with per-interface overrides. |
| No status output. | Live JSON group table: `igmpproxy -S`, and the status pages in LuCI and OPNsense. |
| — | `upstreamleavedelay`: stays joined upstream for a while, so switching back to a channel is instant. |
| — | Upstream IGMP version can be forced (v2/v3) from the UI. |

### Fast leave modes

- **off**: RFC 2236 behaviour. After a leave, the proxy sends `lastmembercount` group-specific
  queries, `lastmemberinterval` apart. If nobody answers, it stops forwarding on that interface.
  Worst-case stop time = count × interval (default 2 × 1 s).
- **on**: stops forwarding on that interface as soon as any host leaves. The queries are still
  sent, so other receivers on the same segment re-report and get the stream back within
  milliseconds. Best when each port has one receiver, such as IPTV set-top boxes.
- **auto**: stops at once when the leaving host was the last host the proxy saw on that
  interface. Otherwise it behaves like `off`. Hosts must not suppress each other's reports, which
  is the case behind a switch with IGMP snooping. If more than `hostlimit` hosts join, the proxy
  falls back to `off` for that group and interface.

## Layout

```
configure.ac, src/, doc/        the daemon (igmpproxy fork, upstream history kept)
openwrt/net/igmpproxy-plus/     OpenWrt package, alternative to "igmpproxy"
openwrt/luci/luci-app-igmpproxy-plus/   LuCI settings + live status pages
opnsense/net/igmp-proxy-plus/   OPNsense plugin (os-igmp-proxy-plus)
opnsense/port/net/igmpproxy-plus/      FreeBSD port for the daemon
tools/nstest.py                 network namespace test (needs root, Linux)
```

Licences: the daemon is GPL-2.0-or-later (see `COPYING`, `Stanford.txt`), the LuCI app is
Apache-2.0, and the OPNsense plugin is BSD-2-Clause.

## Configuration reference

Times in `igmpproxy.conf` accept `500ms`, `2s`, a plain number of seconds (`0.5`) or `2m`.
**Global options must come before the first `phyint`.** Options inside a `phyint` block
override the global value for that interface.

| igmpproxy.conf | OpenWrt UCI (`/etc/config/igmpproxy`) | OPNsense field | Default |
|---|---|---|---|
| `fastleave off\|on\|auto` (global + phyint) | `fastleave` | Fast leave | off |
| `lastmemberinterval` (global + phyint) | `last_member_interval` (ms) | Last member query interval (ms) | 1s |
| `lastmembercount` (global + phyint) | `last_member_count` | Last member query count | robustness |
| `robustness` (global + phyint) | `robustness` | Robustness | 2 |
| `queryinterval` | `query_interval` (s) | Query interval (s) | 125s |
| `queryresponseinterval` | `query_response_interval` (s) | Query response interval (s) | 10s |
| `startupqueryinterval` / `startupquerycount` | `startup_query_interval` / `startup_query_count` | Startup query interval / count | QI/4, robustness |
| `grouptimeout` (phyint) | `group_timeout` (s) | Group membership timeout (s) | robustness×QI+QRI |
| `upstreamleavedelay` | `upstream_leave_delay` (ms) | Upstream leave delay (ms) | 0 |
| `hostlimit` | `host_limit` | Tracked hosts limit | 256 |
| `controlsocket` | (fixed `/var/run/igmpproxy.sock`) | (fixed `/var/run/igmpproxy-plus.sock`) | /var/run/igmpproxy.sock |
| — | `upstream_igmp_version` (sets `force_igmp_version` on the upstream device) | System IGMP version (`net.inet.igmp.default_version`) | auto |
| `blacklist 239.255.255.250/32` added to each downstream | `proxy_ssdp` (0 = keep SSDP local) | Proxy SSDP | SSDP not proxied |
| `quickleave` (legacy) | `quickleave` (legacy) | — | = `fastleave auto` |
| `hashtablesize` (legacy) | — | — | ignored |

Your existing OpenWrt `/etc/config/igmpproxy` works unchanged. `quickleave '0'` means
fast leave is off, but leaves are now detected in about 2 s instead of about 20 s, because
the 1 s last member interval replaces the old 10 s.

## Building

### Daemon on Linux

```sh
./autogen.sh && ./configure && make
sudo ./src/igmpproxy -t igmpproxy.conf        # config check
sudo ./src/igmpproxy -d -v -v igmpproxy.conf   # foreground with debug logging
sudo ./src/igmpproxy -S                        # live group table (JSON)
sudo python3 tools/nstest.py ./src/igmpproxy plus-auto "fastleave auto"   # namespace test
```

### OpenWrt (SDK matching your router's release and target)

The package downloads the tagged release `v$(PKG_VERSION)` from GitHub. Clone this repository
and add its `openwrt/` folder as a feed in the SDK:

```sh
git clone https://github.com/Alpaca-The-Peacemaker/igmpproxy-plus.git
echo "src-link igmpplus $PWD/igmpproxy-plus/openwrt" >> feeds.conf.default
./scripts/feeds update -a && ./scripts/feeds install igmpproxy-plus luci-app-igmpproxy-plus
make package/igmpproxy-plus/compile package/luci-app-igmpproxy-plus/compile V=s
```

To build from a local checkout instead of a release, add
`USE_SOURCE_DIR=/path/to/igmpproxy-plus` to the `make package/igmpproxy-plus/compile` line.

On the router: remove `igmpproxy`, then install `igmpproxy-plus` and `luci-app-igmpproxy-plus`
(apk on 25.12, opkg on 24.10). The service is still `/etc/init.d/igmpproxy` and your existing
`/etc/config/igmpproxy` keeps working. Settings are under **Services → IGMP Proxy**.

### OPNsense (FreeBSD build host with the same major version as OPNsense, e.g. FreeBSD 14)

```sh
cp -r opnsense/port/net/igmpproxy-plus /usr/ports/net/
cd /usr/ports/net/igmpproxy-plus && make makesum && make package
# plugin: inside a checkout of github.com/opnsense/plugins
cp -r opnsense/net/igmp-proxy-plus net/ && cd net/igmp-proxy-plus && make package
```

On OPNsense: uninstall `os-igmp-proxy`, run `pkg add` on both packages, and the plugin appears
under **Services → IGMP Proxy Plus** (Settings and Status). You still need firewall rules that
allow IGMP and the multicast UDP traffic, with "allow options" set, just like with the stock
plugin.

## Credits

igmpproxy-plus builds on the work of the igmpproxy authors and keeps their full history and
copyright notices:

- **Johnny Egeland**, who wrote igmpproxy
- **Pali Rohár** and the igmpproxy contributors, who maintained it
  ([pali/igmpproxy](https://github.com/pali/igmpproxy))
- **Carsten Schill** (smcroute) and the **mrouted** authors, whose code igmpproxy derives from

This is an independent fork, not endorsed by or affiliated with the igmpproxy project. Please
report problems with igmpproxy-plus here, not to the upstream project.

## Roadmap

- IGMPv3 queries (so hosts stay in IGMPv3, no report suppression), with IGMPv2 fallback
- Build and test the OPNsense plugin and FreeBSD port
- Source-specific multicast (RFC 4605 source filtering), only if someone needs it

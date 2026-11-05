# Plan: IGMPv3 support (target release 1.1.0)

## Why

The proxy sends IGMPv2 queries, so every host falls back to IGMPv2. In v2 a host that
hears another host's report stays silent and may never send a leave (report suppression).
That makes host tracking incomplete on shared segments and `fastleave auto` unreliable
without a snooping switch. IGMPv3 hosts never suppress reports, so sending IGMPv3 queries
fixes this, and it keeps the proxy current as networks move to v3.

## Phase 1 – IGMPv3 querier (do this)

1. **Config**
   - `queryversion 2|3`, global and per downstream `phyint`, default `3`
     (`src/config.c`, `struct IfDesc`, `struct Config`).
   - OpenWrt UCI `query_version` (global and per interface) in `openwrt/net/igmpproxy-plus/files/igmpproxy.init`,
     LuCI field in `settings.js` (Querier timers tab and the interface dialog).
   - OPNsense model field `query_version`, forms and the config template.
2. **Sending v3 queries** (`src/igmp.c`, `src/request.c`)
   - 12-byte query, type 0x11: Max Resp Code (RFC 3376 4.1.1: values >= 128 use the
     exponent/mantissa format), group, S flag 0, QRV = robustness (max 7),
     QQIC = query interval in seconds (same float format above 128), number of sources 0.
   - `sendIgmp()` already takes a data length; add the 4 extra bytes and fix the checksum.
   - General and group-specific queries both switch to v3 when the interface is v3.
3. **Older-host compatibility** (RFC 3376 7.3.2), per group per interface
   - When a v1/v2 report arrives, set "older host present" = robustness × QI + QRI.
   - While it runs, the group behaves as v2 there: leaves from v2 hosts are handled as
     today, and v3 BLOCK records are ignored for that group.
   - Show it in the status JSON per member (`"compat": "v2"` or `"v3"`) and in LuCI and OPNsense.
4. **Status**: add `query_version` per interface to `igmpproxy -S`.
5. **Tests** (`tools/nstest.py`)
   - Scenario B with simultaneous joins (no staggering): both receivers must be tracked
     and the first receiver's leave must be counted.
   - A v2 host (`force_igmp_version=2` on one receiver) on a v3 interface: it must keep
     receiving, and the "older host present" state must show up.
   - Keep the existing stop-latency checks for off/on/auto.
6. **Docs**: README note about report suppression becomes "only with `queryversion 2`";
   config reference rows; man page `doc/igmpproxy.conf.5.in`.
7. **Release**: bump to 1.1.0 (OpenWrt Makefile, `configure.ac`), tag `v1.1.0`, check all
   CI jobs, test the installer upgrade and rollback on openwrt-test.

## Phase 2 – source-specific multicast (only if someone needs it)

RFC 4605 proxying with source lists: INCLUDE/EXCLUDE state per group per interface,
per-source forwarding, and upstream joins with source filters
(`MCAST_JOIN_SOURCE_GROUP` / `setsourcefilter`, Linux and FreeBSD). Larger state machine,
about a week of work. Only needed for SSM channels (232/8) or source filtering.

## Other open items (not IGMPv3)

- Real-router test of 1.0.2: `igmpproxy-plus-install` upgrade, `status`, `rollback`.
- OpenWrt pull requests: branches are ready on the forks
  `Alpaca-The-Peacemaker/packages` (branch `igmpproxy-plus`) and
  `Alpaca-The-Peacemaker/luci` (branch `luci-app-igmpproxy-plus`), signed off as
  Dave Ferreira. The user opens them personally (OpenWrt AI policy: no agent-opened PRs,
  disclose AI use). LuCI PR as draft until the package is merged.
  After 1.1.0, bump PKG_VERSION/PKG_HASH in the packages branch.
- OPNsense: add a FreeBSD build job to `.github/workflows/release.yml` (FreeBSD VM action,
  14.x and/or 15.x to match OPNsense), produce the daemon and plugin `.pkg` files, test on
  the OPNsense VM (Proxmox ID 100, links down), then PRs to `opnsense/ports` and
  `opnsense/plugins` (also opened by the user).

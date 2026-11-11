# Plan for 1.6.0: optional querier mode, stalled streams, VLAN support

Status: implemented in 1.6.0 (2026-11-10). Deviations from the plan, all deliberate:

- Feature A step 3: querier interfaces **do** get a kernel VIF. Without one, Linux delivers an
  IGMPv2 report for a routable group to the daemon only if the host itself is a member of
  that group, so most reports would never arrive. No route ever forwards to or from the VIF.
- Feature A step 4/5: a small group table per querier interface (groups, last reporter,
  membership timer) instead of "event only". Hosts repeat their leave (robustness), and
  without the table every repeat started a new round of group specific queries. The table
  also feeds the status JSON (`groups` per querier interface) and the Status page.
- Feature D step 1: the multi-upstream test found a real bug. The kernel upcall picked the
  upstream from the source address, the first upstream whose altnet matched; with two
  upstreams and overlapping altnets (0.0.0.0/0) the second upstream's streams got a route
  expecting them on the first and were never forwarded. Fixed by using the VIF the kernel
  reports in the upcall (`im_vif`). The test fails on 1.5.0 and passes on 1.6.0.
- Feature D step 6: no `multicast_to_unicast_all` check. With bridge snooping on, OpenWrt
  already converts multicast to unicast for Wi-Fi clients by default; the page warns only when
  snooping is off or a Wi-Fi interface has `multicast_to_unicast` explicitly set to 0.

Written 2026-11-10 from a read of the code at e2afc6c.

## What exists today

- The proxy already sends general queries on every **downstream** interface
  (`sendGeneralQueries`, src/request.c:239) and group specific queries after a leave. It has to:
  that is how it learns who is still watching.
- It never takes part in **querier election**: received queries are dropped
  (`case IGMP_MEMBERSHIP_QUERY: return;`, src/igmp.c:247), so it keeps querying even when
  another querier with a lower address is on the same LAN.
- It cannot run **without an upstream**: "There must be at least 1 Vif as upstream"
  (src/igmpproxy.c:304), and reports are only accepted on downstream interfaces
  (src/request.c:102 and :144).
- Interface roles are `upstream | downstream | disabled` (src/config.c:559-567,
  `IF_STATE_*` in src/igmpproxy.h:130).

So "querier mode" means two new, separate things. Both are off by default.

## Feature A: `querier` interface role (query only, no forwarding)

Use: a LAN or VLAN with snooping switches but no multicast router (cameras, Sonos, AV gear).
Snooping needs a querier or the switches flood or prune wrongly. The interface gets queries
and nothing else: no upstream join, no kernel route, no forwarding.

1. src/igmpproxy.h: add `#define IF_STATE_QUERIER 5`.
2. src/config.c (phyint parser, near line 559): accept the token `querier`.
3. src/igmpproxy.c:270-305: do not call `addVIF` for querier interfaces (no kernel VIF
   needed). Change the upstream check: an error only if there is a downstream interface and no
   upstream; a config with only querier interfaces is valid. Check that the mroute socket
   init (`initMRouter`) is skipped or harmless in that case.
4. src/request.c: `sendGeneralQueries` (line 245) also sends on `IF_STATE_QUERIER`.
   Group specific queries are not needed there (no leave processing).
5. src/request.c:102 and :144 (`acceptGroupReport`, `acceptLeaveMessage`): on a querier
   interface, do not create routes. Only log an event (`report` / `leave` with
   `"role":"querier"`) so the Events page shows who answered. Keep the existing "ignoring"
   path for upstream and disabled.
6. src/rttable.c:125 (`initRouteTable`): join all-routers (224.0.0.2) and 224.0.0.22 on
   querier interfaces too, so reports and leaves reach the daemon.
7. src/rttable.c:833-837 (status JSON): report state `"querier"`.
8. src/ifvc.c:69, :150-168: the LOST/HIDDEN logic only handles downstream; make sure a
   querier interface keeps its role across an interface rescan.

## Feature B: querier election (RFC 2236 section 3, RFC 3376 section 6.6.2)

Option `querierelection on|off`, global and per interface, default **off** (today's behaviour:
always query). When on, for downstream and querier interfaces:

1. src/igmp.c:247: pass received general queries to a new
   `acceptQuery(src, dst, group, igmp)` in src/request.c instead of dropping them. Ignore our
   own address and source 0.0.0.0.
2. Per interface state in `struct IfDesc`: `uint32_t otherQuerier; msec_t otherQuerierUntil;`.
   If the query's source address is numerically lower than ours: store it and set
   `otherQuerierUntil = now + robustness * queryInterval + queryResponseInterval / 2`
   (Other Querier Present Interval). A higher address is ignored.
3. `sendGeneralQueries`: skip an interface while `otherQuerierUntil > now`. When it expires,
   log and resume at the next periodic tick (optionally send one query at once).
4. Group specific queries after a leave: RFC says only the querier sends them. As
   non-querier we still must learn memberships, which works by listening to reports answering
   the other querier. On a **downstream** interface keep sending our own group specific
   queries regardless (safe: fast leave correctness matters more than strict RFC); document it.
5. Events: `querier_lost` (other querier seen, with its address) and `querier_resumed`.
   Status JSON per interface: `"querier":"self"|"<address>"`.
6. `-Q` (refresh) always sends, even as non-querier.

## Configuration

- igmpproxy.conf: `phyint <if> querier`; `querierelection on|off` (global and under phyint).
- doc/igmpproxy.conf.5.in: document both.
- OpenWrt: openwrt/net/igmpproxy-plus/files/igmpproxy.init: `direction` accepts `querier`
  (lines 104-129: no altnet/filters emitted, no forwarding firewall rule; only the IGMP input
  rule for the zone, see lines 157-195); new UCI option `querier_election` (bool, default 0).
  LuCI settings.js: third value in the Direction list ("Querier only (no forwarding)"),
  a flag "Querier election" on the Advanced tab; status.js shows the role and the active
  querier.
- OPNsense: model `direction` gets `querier`, general `querier_election`; template and
  dialogInterface.xml to match.

## Feature C: stalled-stream detection in the daemon

Today only the LuCI status page detects it (status.js `stalled()`), so nothing is recorded
when no browser is open.

1. src/rttable.c: per route keep `lastBytes`, `lastBytesAt`, `stalledSince`, `peakBps`.
   Sample `getSGCount` from the existing aging tick (`ageActiveRoutes`), at most every 2 s
   per route; do not add a new timer per route.
2. A route is stalled when: upstream joined, at least one vif forwarding, it carried at least
   500 kbit/s before, and the byte counter has not moved for `stalltimeout` (new global
   option, default 10 s, 0 = off).
3. Events `stream_stalled` (group, upstream if, `silent_s`) and `stream_resumed`
   (`stalled_s`). Status JSON per group: `"stalled_ms"` (-1 when not stalled).
4. LuCI: status.js uses `stalled_ms` instead of its own bookkeeping; common.js `analyze`
   lists `stream_stalled` as a problem; events.js describes both events.
5. Test: in eventlog-test.py stop the sender for 12 s while a receiver stays joined, expect
   `stream_stalled`, restart it, expect `stream_resumed`.

## Feature D: VLAN support in the interface (IPTV, IoT, guest VLANs)

The daemon options exist (per phyint whitelist/blacklist, several upstreams, per interface
leave settings). This is about verifying them and making them usable.

1. **Verify multiple upstreams first.** New tools/multiupstream-test.py: two upstream
   namespaces, whitelist A on one and B on the other; a join for a group in A must be sent
   only to upstream A and forwarded from it, same for B; a group in neither is not joined
   anywhere. Fix what the test finds before any UI work (`sendJoinLeaveUpstream`,
   src/rttable.c:140-195, and `upStreamIfIdx`, MAX_UPS_VIFS).
2. **Per interface policy, simple choices** (LuCI settings.js, Interfaces > Edit, and the
   OPNsense interface dialog). A list "What this network may request":
   - everything (no filter, today's default)
   - only the channel list (whitelist = `common.whitelistFromChannels`)
   - nothing from the provider (downstream becomes role `querier`, Feature A)
   - custom (shows the existing whitelist/blacklist lists)
   It only writes the existing `whitelist` / `blacklist` / `direction` options, no new UCI
   semantics. Keep the warning that set-top boxes may need groups outside a channel list.
3. **Guided IPTV upstream.** In settings.js a small dialog "Add a TV network": pick the
   network (e.g. the provider's IPTV VLAN), enter the source networks (altnet), optional
   channel-list whitelist; it creates the `phyint` upstream section and sets its zone so
   the init script emits the firewall rules (igmpproxy.init:157-195). Nothing is applied
   until Save & Apply.
4. **Status page per network.** status.js: group Devices and Streams by downstream
   interface when there is more than one, with each network's role, leave settings and
   active querier (Feature B); bandwidth totals per network. One downstream = today's
   layout, unchanged.
5. **Events per network.** events.js filter by interface; `analyze` names the network in
   its messages.
6. **Checks with advice (read only, OpenWrt).** A small rpcd-readable helper or `fs.read`
   of `/sys/class/net/<bridge>/bridge/multicast_snooping` for bridge downstreams, and
   `uci get wireless.*.multicast_to_unicast_all` for Wi-Fi networks attached to them:
   warn on the status page when snooping is off or a Wi-Fi network carries streams without
   multicast-to-unicast. Never change these settings from the plugin; show the command or
   the LuCI path. Needs matching read ACL entries.

Not in this plan (only if users ask): IGMPv3 source filtering, MLD / IPv6.

## Tests (required before release)

- New tools/querier-test.py (namespaces from nstest.py):
  1. config with one `querier` interface and no upstream starts; a general query is seen on
     the interface every query interval (tcpdump or a receiver socket); no upstream join, no
     kernel route (`/proc/net/ip_mr_cache` empty) after a host joins.
  2. election on: a second querier with a lower address sends queries -> ours stop within one
     interval, event `querier_lost`; it stops -> ours resume after the Other Querier Present
     Interval, event `querier_resumed`. A querier with a higher address changes nothing.
  3. election off (default): behaviour identical to 1.5.1 (ours keep querying).
- Existing suites must pass unchanged: compat-test.py (60 cases, incl. MUTED_CASE),
  eventlog-test.py, linklocal-test.py, robustness-test.py, scale-test.py,
  test-installer.sh, luci-common-test.js, tools/freebsd-jail-test.sh.
- Add the new test to .github/workflows/release.yml (compat job).

## Rules for whoever implements this

- Defaults must not change behaviour: no `querier` interface and election off = 1.5.1.
- Never reintroduce the "heard within one query cycle" cut (see compat-test MUTED_CASE).
- Patch files through script files, not shell heredocs (Git Bash eats backslashes).
- Build and test on CT 107 (start it in Proxmox if stopped); do not test on the production
  router. Release as 1.6.0 only after all suites pass.

## Order of work

1. Feature A daemon + test 1. 2. Feature B daemon + tests 2 and 3. 3. Feature C daemon +
test. 4. Feature D step 1 (multi-upstream test and fixes). 5. OpenWrt init + LuCI for A, B, C
and D steps 2 to 6; extend tools/luci-common-test.js for every new pure helper. 6. OPNsense
model/template/dialog. 7. README: "Use it as a querier only" and "Networks with VLANs".
8. Release 1.6.0.

The LuCI pages have no browser test: after step 5 ask the user to click through Status,
Events and Settings once before the release.

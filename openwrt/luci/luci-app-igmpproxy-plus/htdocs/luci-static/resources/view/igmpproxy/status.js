'use strict';
'require view';
'require fs';
'require poll';
'require ui';
'require uci';
'require igmpproxy.common as common';

/* Local discovery groups, shown under "Other" rather than as streams. */
var KNOWN = {
	'239.255.255.250': _('UPnP / SSDP discovery'),
	'224.0.0.251': _('mDNS / Bonjour'),
	'224.0.0.252': _('LLMNR'),
	'224.0.1.187': _('CoAP (smart home discovery)')
};

var ISSUE_HOURS = 24;

/* exec_direct streams over HTTP: no ubus message size limit (about 1 MB,
 * reached with a few thousand streams) */
function fetchStatus() {
	return fs.exec_direct('/usr/sbin/igmpproxy', ['-S'], 'text').then(function(out) {
		if (!out || out.charAt(0) != '{')
			return { error: (out || '').trim() || _('IGMP proxy is not running') };
		try {
			return JSON.parse(out);
		}
		catch (e) {
			return { error: _('Invalid status data: %s').format(e.message) };
		}
	}).catch(function(e) {
		return { error: e.message || _('IGMP proxy is not running') };
	});
}

/*
 * Read-only checks with advice: multicast snooping on the bridges the
 * streams go to, and Wi-Fi networks on them with multicast-to-unicast
 * switched off. Nothing is changed from here.
 */
function fetchChecks(st) {
	var nets = common.networksOf(st);
	return Promise.all(nets.map(function(i) {
		return L.resolveDefault(fs.read('/sys/class/net/' + i.name + '/bridge/multicast_snooping'), null)
			.then(function(v) { return { dev: i.name, snooping: v == null ? null : String(v).trim() }; });
	})).then(function(snoop) {
		var advice = [];
		snoop.forEach(function(s) {
			if (s.snooping === '0')
				advice.push(_('Multicast snooping is off on the bridge %s: every stream goes to every port and every Wi-Fi client on it. Switch it on under Network > Interfaces > Devices > %s > Bridge port specific options ("Enable IGMP snooping"), or run: uci set network.<device section>.igmp_snooping=1; uci commit network; service network reload. With snooping on, OpenWrt also delivers streams to Wi-Fi clients one by one, which Wi-Fi handles much better.').format(s.dev, s.dev));
		});
		var down = uci.sections('igmpproxy', 'phyint').filter(function(p) {
			return p.direction == 'downstream' || p.direction == 'querier';
		}).map(function(p) { return p.network; });
		uci.sections('wireless', 'wifi-iface').forEach(function(w) {
			var wn = L.toArray(w.network);
			var shared = wn.filter(function(n) { return down.indexOf(n) >= 0; });
			if (shared.length && w.multicast_to_unicast == '0')
				advice.push(_('Wi-Fi network "%s" on %s has multicast-to-unicast switched off: TV streams over this Wi-Fi will break up. Remove the option: uci delete wireless.%s.multicast_to_unicast; uci commit wireless; wifi').format(w.ssid || w['.name'], shared.join(', '), w['.name']));
		});
		return advice;
	});
}

function dur(ms) {
	if (ms == null || ms < 0)
		return '-';
	var s = Math.round(ms / 1000);
	if (s < 60)
		return s + ' s';
	if (s < 3600)
		return Math.floor(s / 60) + ' min';
	var h = Math.floor(s / 3600);
	return h + ' h ' + Math.floor((s % 3600) / 60) + ' min';
}

function secs(ms) {
	return (ms / 1000).toFixed(1) + ' s';
}

function badge(text, color) {
	return E('span', {
		'style': 'display:inline-block;padding:0 .45em;border-radius:.6em;font-size:85%;' +
		         'border:1px solid ' + color + ';color:' + color + ';white-space:nowrap'
	}, text);
}

function dot(color) {
	return E('span', { 'style': 'color:' + color + ';margin-right:.35em' }, '●');
}

function device(ip, names) {
	var n = names[ip];
	return n ? E('span', {}, [E('strong', {}, n), ' ', E('small', { 'style': 'opacity:.6' }, ip)])
	         : E('strong', {}, ip);
}

function table(headers, rows, empty) {
	var t = E('table', { 'class': 'table' }, [
		E('tr', { 'class': 'tr table-titles' }, headers.map(function(h) {
			return E('th', { 'class': 'th' }, h);
		}))
	]);
	if (!rows.length)
		t.appendChild(E('tr', { 'class': 'tr placeholder' },
			E('td', { 'class': 'td', 'colspan': headers.length }, E('em', {}, empty))));
	rows.forEach(function(r) {
		t.appendChild(E('tr', { 'class': 'tr' }, r.map(function(c) {
			return E('td', { 'class': 'td' }, c);
		})));
	});
	return t;
}

/* Plain-language state of one stream, over all downstream interfaces:
 * the most active member decides (streaming > checking > stopped). */
function channelState(g) {
	if (!g.members.length)
		return [dot('#999'), _('no receivers')];
	var fwd = g.members.filter(function(m) { return m.forwarding; });
	if (!fwd.length)
		return [dot('#999'), _('stopped, waiting for a device to answer')];
	if (g.stalled_ms >= 0)
		return [dot('#c0392b'), _('no data from the provider for %d s').format(Math.round(g.stalled_ms / 1000))];
	if (g.leave_in_ms >= 0)
		return [dot('#e0a000'), _('stopping in %s').format(secs(g.leave_in_ms))];
	var steady = fwd.filter(function(m) {
		return !(m.last_member_queries_left > 0) && m.expires_in_ms >= 5000;
	});
	if (!steady.length) {
		var t = Math.max.apply(null, fwd.map(function(m) { return m.expires_in_ms; }));
		return [dot('#e0a000'), _('a device left, checking for others (%s)').format(secs(t))];
	}
	return [dot('#2e9e44'), _('streaming')];
}

/*
 * Bit rate per stream from two samples of the kernel byte counters.
 * Returns { group: bits per second } for the groups that have a rate.
 */
var lastSample = {};
function rates(st) {
	var now = Date.now(), out = {}, next = {};
	(st.groups || []).forEach(function(g) {
		if (g.bytes == null)
			return;
		next[g.group] = { bytes: g.bytes, t: now };
		var p = lastSample[g.group];
		if (p && g.bytes >= p.bytes && now > p.t)
			out[g.group] = (g.bytes - p.bytes) * 8 * 1000 / (now - p.t);
	});
	lastSample = next;
	return out;
}

function querierText(i) {
	if (!i.querier)
		return '-';
	return i.querier === 'self' ? _('this router') : _('%s (other router)').format(i.querier);
}

/* Devices and Streams tables for some streams, seen from one network
 * (ifname) or from all of them (ifname null). */
function streamTables(groups, ifname, names, channels, rate) {
	var byHost = {};
	groups.forEach(function(g) {
		g.members.forEach(function(m) {
			if (ifname && m.interface !== ifname)
				return;
			m.hosts.forEach(function(h) {
				var d = byHost[h.address] = byHost[h.address] || { channels: [], v2: false };
				if (d.channels.indexOf(g.group) < 0)
					d.channels.push(g.group);
				if (m.compat === 'v2')
					d.v2 = true;
			});
		});
	});
	var hosts = Object.keys(byHost).sort();

	var watchRows = hosts.map(function(ip) {
		var h = byHost[ip], bw = 0, known = false;
		h.channels.forEach(function(g) {
			if (rate[g] != null) { bw += rate[g]; known = true; }
		});
		return [
			device(ip, names),
			E('span', {}, h.channels.map(function(g) {
				return E('span', { 'style': 'margin-right:.8em;white-space:nowrap' }, common.channel(g, channels));
			})),
			common.mbps(known ? bw : null),
			h.v2 ? badge('IGMPv2', '#888') : badge('IGMPv3', '#2e7bcf')
		];
	});

	var chanRows = groups.map(function(g) {
		var viewers = [];
		g.members.forEach(function(m) {
			if (ifname && m.interface !== ifname)
				return;
			m.hosts.forEach(function(h) { viewers.push(names[h.address] || h.address); });
			if (m.hosts_untracked)
				viewers.push(_('more'));
		});
		return [
			common.channel(g.group, channels),
			E('span', {}, channelState(g)),
			common.mbps(rate[g.group]),
			viewers.length ? viewers.join(', ') : E('em', {}, _('none')),
			g.start_delay_ms >= 0 ? secs(g.start_delay_ms) : '-',
			dur(g.age_ms)
		];
	});

	return {
		hosts: hosts.length,
		nodes: [
			E('h4', {}, _('Devices')),
			table([_('Device'), _('Streams'), _('Bandwidth'), _('IGMP')], watchRows, _('No device is receiving a stream right now')),
			E('h4', {}, _('Streams')),
			table([_('Stream'), _('State'), _('Bit rate'), _('Receiving'), _('Started in'), _('Running for')], chanRows, _('No streams right now'))
		]
	};
}

function render(st, names, channels, rate, issues, advice) {
	if (st.error)
		return E('div', { 'class': 'alert-message warning' }, st.error);

	var c = st.config;
	var tv = [], other = [];
	st.groups.forEach(function(g) {
		var local = KNOWN[g.group] || /^224\.0\.0\./.test(g.group);
		(local ? other : tv).push(g);
	});

	var total = 0, measured = false;
	tv.forEach(function(g) {
		if (rate[g.group] != null) {
			total += rate[g.group];
			measured = true;
		}
	});

	var hostSet = {};
	tv.forEach(function(g) { g.members.forEach(function(m) { m.hosts.forEach(function(h) { hostSet[h.address] = true; }); }); });

	var stopAfter = c.fastleave === 'on' ? _('instantly')
		: c.fastleave === 'auto' ? _('instantly (IGMPv3 boxes) or after %s').format(secs(c.last_member_query_count * c.last_member_query_interval_ms))
		: _('%s after the last device leaves').format(secs(c.last_member_query_count * c.last_member_query_interval_ms));

	var summary = E('div', { 'class': 'cbi-section', 'style': 'padding:.8em 1em;font-size:110%' }, [
		dot('#2e9e44'), E('strong', {}, _('Running')), ' · ',
		_('%d device(s) receiving %d stream(s)').format(Object.keys(hostSet).length, tv.length),
		tv.length ? E('span', {}, [' · ', E('strong', {}, common.mbps(measured ? total : null)), ' ', _('from the provider')]) : '',
		' · ', _('streams stop %s').format(stopAfter),
		E('div', { 'style': 'font-size:85%;opacity:.7;margin-top:.3em' },
			_('Fast leave: %s · IGMPv%d queries · querier election %s · up %s').format(
				c.fastleave, c.query_version || 2, c.querier_election ? _('on') : _('off'), dur(st.uptime_ms)))
	]);

	var joinFail = st.upstream_join_failures ? E('div', { 'class': 'alert-message danger', 'style': 'margin-bottom:1em' },
		_('%d upstream join(s) were refused by the system since the proxy started: those streams were not requested from the provider. See the Events page and the system log.').format(st.upstream_join_failures)) : '';

	var stalled = tv.filter(function(g) { return g.stalled_ms >= 0; });
	var stallLine = stalled.length ? E('div', { 'class': 'alert-message danger', 'style': 'margin-bottom:1em' },
		_('No data is arriving from the provider for: %s. Devices are asking for it and the proxy has requested it, so the cause is upstream (provider, modem or the cable to it).').format(
			stalled.map(function(g) { return common.channelText(g.group, channels); }).join(', '))) : '';

	var issueLine = '';
	if (issues && issues.length)
		issueLine = E('div', { 'class': 'alert-message warning', 'style': 'margin-bottom:1em' }, [
			_('%d possible problem(s) in the last %d hours, e.g.: %s').format(issues.length, ISSUE_HOURS, issues[0].text), ' ',
			E('a', { 'href': L.url('admin/services/igmpproxy/events') }, _('Details'))
		]);

	var adviceBox = (advice && advice.length) ? E('div', { 'class': 'alert-message notice', 'style': 'margin-bottom:1em' },
		E('ul', { 'style': 'margin:0 0 0 1.2em' }, advice.map(function(a) { return E('li', {}, a); }))) : '';

	/* one network: the classic layout; several: one block per network */
	var nets = common.networksOf(st), body = [];
	if (nets.length <= 1) {
		var all = streamTables(tv, null, names, channels, rate);
		body = all.nodes;
		if (nets.length && nets[0].state === 'querier')
			body = [querierBlock(nets[0], names, channels)];
	} else {
		nets.forEach(function(i) {
			if (i.state === 'querier') {
				body.push(E('h3', {}, _('Network %s').format(i.name)));
				body.push(querierBlock(i, names, channels));
				return;
			}
			var mine = common.groupsOn(tv, i.name), bw = 0, known = false;
			mine.forEach(function(g) { if (rate[g.group] != null) { bw += rate[g.group]; known = true; } });
			var t = streamTables(mine, i.name, names, channels, rate);
			body.push(E('h3', {}, _('Network %s').format(i.name)));
			body.push(E('p', { 'style': 'opacity:.75' }, _('%d device(s), %d stream(s), %s · fast leave %s · querier: %s').format(
				t.hosts, mine.length, common.mbps(known ? bw : null), i.fastleave, querierText(i))));
			body = body.concat(t.nodes);
		});
	}

	var otherRows = other.map(function(g) {
		var n = 0;
		g.members.forEach(function(m) { n += m.hosts.length; });
		return [E('code', {}, g.group), KNOWN[g.group] || _('local network'), String(n)];
	});

	var ifRows = st.interfaces.map(function(i) {
		var role = i.state === 'upstream' ? _('Upstream (towards the provider)')
			: i.state === 'querier' ? _('Querier only (no forwarding)')
			: _('Downstream (towards your devices)');
		return [
			i.name, i.address, role,
			i.state === 'downstream' ? _('%s, checks %d × %d ms, timeout %s').format(i.fastleave, i.last_member_query_count, i.last_member_query_interval_ms, dur(i.group_membership_interval_ms))
				: i.state === 'querier' ? _('checks %d × %d ms').format(i.last_member_query_count, i.last_member_query_interval_ms) : '-',
			querierText(i)
		];
	});

	return E('div', {}, [
		summary,
		joinFail,
		stallLine,
		issueLine,
		adviceBox
	].concat(body).concat([
		E('p', { 'style': 'font-size:85%;opacity:.7' }, Object.keys(channels).length
			? _('Channel names from your channel list (Settings > Channel names).')
			: _('Tip: add your provider\'s channel list under Settings > Channel names to see names instead of addresses.')),
		E('details', { 'style': 'margin-top:1.5em' }, [
			E('summary', { 'style': 'cursor:pointer' }, _('Details: interfaces and other multicast')),
			table([_('Interface'), _('Address'), _('Role'), _('Leave settings'), _('Querier')], ifRows, _('No interfaces')),
			otherRows.length ? table([_('Group'), _('What it is'), _('Devices')], otherRows, '') : ''
		])
	]));
}

/* A querier-only network: who answered, nothing is forwarded. */
function querierBlock(i, names, channels) {
	var rows = (i.groups || []).map(function(g) {
		return [
			KNOWN[g.group] ? E('span', {}, [E('code', {}, g.group), ' ', KNOWN[g.group]]) : common.channel(g.group, channels),
			device(g.last_reporter, names),
			g.checking ? _('checking whether anyone still wants it') : _('expires in %s').format(dur(g.expires_in_ms))
		];
	});
	return E('div', {}, [
		E('p', { 'style': 'opacity:.75' }, _('Querier only: this router asks the devices on %s which groups they want, so the switches there deliver multicast only to the ports that asked. Nothing is forwarded to or from this network. Active querier: %s.').format(i.name, querierText(i))),
		table([_('Group'), _('Last answer from'), _('State')], rows, _('No device has answered yet'))
	]);
}

return view.extend({
	load: function() {
		return Promise.all([
			fetchStatus(), common.fetchNames(), common.loadChannels(), common.fetchEvents(),
			L.resolveDefault(uci.load('igmpproxy'), null), L.resolveDefault(uci.load('wireless'), null)
		]).then(function(data) {
			return L.resolveDefault(fetchChecks(data[0]), []).then(function(advice) {
				data.push(advice);
				return data;
			});
		});
	},

	render: function(data) {
		var st = data[0], names = data[1], channels = data[2], advice = data[6];
		var issues = common.analyze(data[3].events || [], names, channels, ISSUE_HOURS);
		var container = E('div', { 'id': 'igmpproxy-status' }, render(st, names, channels, rates(st), issues, advice));

		poll.add(function() {
			return fetchStatus().then(function(st) {
				var c = document.getElementById('igmpproxy-status');
				if (!c)
					return;
				// keep the details section open across refreshes
				var d = c.querySelector('details');
				var open = d && d.open;
				c.replaceChildren(render(st, names, channels, rates(st), issues, advice));
				var nd = c.querySelector('details');
				if (open && nd)
					nd.open = true;
			});
		}, 2);

		return E('div', { 'class': 'cbi-map' }, [
			E('h2', {}, _('IGMP Proxy')),
			E('div', { 'class': 'cbi-map-descr' }, _('Which devices (IPTV boxes, speakers, computers...) receive which multicast streams from your provider, live.')),
			E('div', { 'style': 'display:flex;flex-wrap:wrap;gap:.6em;align-items:center;margin-bottom:1em' }, [
				E('button', {
					'class': 'cbi-button cbi-button-action',
					'title': _('Send a query now so every device reports what it receives'),
					'click': ui.createHandlerFn(this, function(ev) {
						var note = ev.target.nextElementSibling;
						return fs.exec('/usr/sbin/igmpproxy', ['-Q']).then(function(res) {
							note.textContent = res.code === 0
								? _('Asked all devices; answers arrive within %d s').format(
									Math.round(((st.config || {}).query_response_interval_ms || 10000) / 1000))
								: _('Could not send the query: %s').format((res.stderr || res.stdout || '').trim());
						});
					})
				}, _('Refresh groups')),
				E('span', { 'style': 'opacity:.7' }),
				E('a', { 'href': L.url('admin/services/igmpproxy/events'), 'style': 'margin-left:auto' }, _('Event history'))
			]),
			container
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});

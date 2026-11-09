'use strict';
'require view';
'require fs';
'require poll';
'require ui';
'require igmpproxy.common as common';

/* Local discovery groups, shown under "Other" rather than as streams. */
var KNOWN = {
	'239.255.255.250': _('UPnP / SSDP discovery'),
	'224.0.0.251': _('mDNS / Bonjour'),
	'224.0.0.252': _('LLMNR')
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

function render(st, names, channels, rate, issues) {
	if (st.error)
		return E('div', { 'class': 'alert-message warning' }, st.error);

	var c = st.config;
	var tv = [], other = [];
	st.groups.forEach(function(g) {
		var local = KNOWN[g.group] || /^224\.0\.0\./.test(g.group);
		(local ? other : tv).push(g);
	});

	/* who is receiving what: device -> streams */
	var byHost = {};
	tv.forEach(function(g) {
		g.members.forEach(function(m) {
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

	var total = 0, measured = false;
	tv.forEach(function(g) {
		if (rate[g.group] != null) {
			total += rate[g.group];
			measured = true;
		}
	});

	var stopAfter = c.fastleave === 'on' ? _('instantly')
		: c.fastleave === 'auto' ? _('instantly (IGMPv3 boxes) or after %s').format(secs(c.last_member_query_count * c.last_member_query_interval_ms))
		: _('%s after the last device leaves').format(secs(c.last_member_query_count * c.last_member_query_interval_ms));

	var summary = E('div', { 'class': 'cbi-section', 'style': 'padding:.8em 1em;font-size:110%' }, [
		dot('#2e9e44'), E('strong', {}, _('Running')), ' · ',
		_('%d device(s) receiving %d stream(s)').format(hosts.length, tv.length),
		tv.length ? [' · ', E('strong', {}, common.mbps(measured ? total : null)), ' ', _('from the provider')] : '',
		' · ', _('streams stop %s').format(stopAfter),
		E('div', { 'style': 'font-size:85%;opacity:.7;margin-top:.3em' },
			_('Fast leave: %s · IGMPv%d queries · up %s').format(c.fastleave, c.query_version || 2, dur(st.uptime_ms)))
	]);

	var joinFail = st.upstream_join_failures ? E('div', { 'class': 'alert-message danger', 'style': 'margin-bottom:1em' },
		_('%d upstream join(s) were refused by the system since the proxy started: those streams were not requested from the provider. See the Events page and the system log.').format(st.upstream_join_failures)) : '';

	var issueLine = '';
	if (issues && issues.length)
		issueLine = E('div', { 'class': 'alert-message warning', 'style': 'margin-bottom:1em' }, [
			_('%d possible problem(s) in the last %d hours, e.g.: %s').format(issues.length, ISSUE_HOURS, issues[0].text), ' ',
			E('a', { 'href': L.url('admin/services/igmpproxy/events') }, _('Details'))
		]);

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

	var chanRows = tv.map(function(g) {
		var viewers = [];
		g.members.forEach(function(m) {
			m.hosts.forEach(function(h) { viewers.push(names[h.address] || h.address); });
			if (m.hosts_untracked)
				viewers.push(_('more'));
		});
		return [
			common.channel(g.group, channels),
			E('span', {}, channelState(g)),
			common.mbps(rate[g.group]),
			viewers.length ? viewers.join(', ') : E('em', {}, _('none')),
			dur(g.age_ms)
		];
	});

	var otherRows = other.map(function(g) {
		var n = 0;
		g.members.forEach(function(m) { n += m.hosts.length; });
		return [E('code', {}, g.group), KNOWN[g.group] || _('local network'), String(n)];
	});

	var ifRows = st.interfaces.map(function(i) {
		return [
			i.name, i.address,
			i.state === 'upstream' ? _('Upstream (towards the provider)') : _('Downstream (towards your devices)'),
			i.state === 'downstream' ? _('%s, checks %d × %d ms, timeout %s').format(i.fastleave, i.last_member_query_count, i.last_member_query_interval_ms, dur(i.group_membership_interval_ms)) : '-'
		];
	});

	return E('div', {}, [
		summary,
		joinFail,
		issueLine,
		E('h3', {}, _('Devices')),
		table([_('Device'), _('Streams'), _('Bandwidth'), _('IGMP')], watchRows, _('No device is receiving a stream right now')),
		E('h3', {}, _('Streams')),
		table([_('Stream'), _('State'), _('Bit rate'), _('Receiving'), _('Running for')], chanRows, _('No streams right now')),
		E('p', { 'style': 'font-size:85%;opacity:.7' }, Object.keys(channels).length
			? _('Channel names from your channel list (Settings > Channel names).')
			: _('Tip: add your provider\'s channel list under Settings > Channel names to see names instead of addresses.')),
		E('details', { 'style': 'margin-top:1.5em' }, [
			E('summary', { 'style': 'cursor:pointer' }, _('Details: interfaces and other multicast')),
			table([_('Interface'), _('Address'), _('Role'), _('Leave settings')], ifRows, _('No interfaces')),
			otherRows.length ? table([_('Group'), _('What it is'), _('Devices')], otherRows, '') : ''
		])
	]);
}

return view.extend({
	load: function() {
		return Promise.all([fetchStatus(), common.fetchNames(), common.loadChannels(), common.fetchEvents()]);
	},

	render: function(data) {
		var st = data[0], names = data[1], channels = data[2];
		var issues = common.analyze(data[3].events || [], names, channels, ISSUE_HOURS);
		var container = E('div', { 'id': 'igmpproxy-status' }, render(st, names, channels, rates(st), issues));

		poll.add(function() {
			return fetchStatus().then(function(st) {
				var c = document.getElementById('igmpproxy-status');
				if (!c)
					return;
				// keep the details section open across refreshes
				var d = c.querySelector('details');
				var open = d && d.open;
				c.replaceChildren(render(st, names, channels, rates(st), issues));
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

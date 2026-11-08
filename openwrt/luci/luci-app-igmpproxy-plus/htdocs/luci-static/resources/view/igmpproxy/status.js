'use strict';
'require view';
'require fs';
'require rpc';
'require poll';
'require ui';

var callLeases = rpc.declare({
	object: 'luci-rpc',
	method: 'getDHCPLeases',
	expect: { '': {} }
});

/* Local discovery groups, shown under "Other" rather than as streams. */
var KNOWN = {
	'239.255.255.250': _('UPnP / SSDP discovery'),
	'224.0.0.251': _('mDNS / Bonjour'),
	'224.0.0.252': _('LLMNR')
};

function fetchStatus() {
	return fs.exec('/usr/sbin/igmpproxy', ['-S']).then(function(res) {
		if (res.code !== 0)
			return { error: (res.stderr || '').trim() || _('IGMP proxy is not running') };
		try {
			return JSON.parse(res.stdout);
		}
		catch (e) {
			return { error: _('Invalid status data: %s').format(e.message) };
		}
	}).catch(function(e) {
		return { error: e.message };
	});
}

function fetchNames() {
	return L.resolveDefault(callLeases(), {}).then(function(res) {
		var names = {};
		(res.dhcp_leases || []).forEach(function(l) {
			if (l.ipaddr && l.hostname)
				names[l.ipaddr] = l.hostname;
		});
		return names;
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

/* Plain-language state of one channel on the LAN. */
function channelState(g) {
	if (!g.members.length)
		return [dot('#999'), _('no receivers')];
	var m = g.members[0];
	if (!m.forwarding)
		return [dot('#999'), _('stopped, waiting for a device to answer')];
	if (g.leave_in_ms >= 0)
		return [dot('#e0a000'), _('stopping in %s').format(secs(g.leave_in_ms))];
	if (m.last_member_queries_left > 0 || m.expires_in_ms < 5000)
		return [dot('#e0a000'), _('a device left, checking for others (%s)').format(secs(m.expires_in_ms))];
	return [dot('#2e9e44'), _('streaming')];
}

function render(st, names) {
	if (st.error)
		return E('div', { 'class': 'alert-message warning' }, st.error);

	var c = st.config;
	var tv = [], other = [];
	st.groups.forEach(function(g) {
		var local = KNOWN[g.group] || /^224\.0\.0\./.test(g.group);
		(local ? other : tv).push(g);
	});

	/* who is watching what: device -> channels */
	var byHost = {};
	tv.forEach(function(g) {
		g.members.forEach(function(m) {
			m.hosts.forEach(function(h) {
				(byHost[h.address] = byHost[h.address] || { channels: [], v2: false }).channels.push(g.group);
				if (m.compat === 'v2')
					byHost[h.address].v2 = true;
			});
		});
	});
	var hosts = Object.keys(byHost).sort();

	var stopAfter = c.fastleave === 'on' ? _('instantly')
		: c.fastleave === 'auto' ? _('instantly (IGMPv3 boxes) or after %s').format(secs(c.last_member_query_count * c.last_member_query_interval_ms))
		: _('%s after the last device leaves').format(secs(c.last_member_query_count * c.last_member_query_interval_ms));

	var summary = E('div', { 'class': 'cbi-section', 'style': 'padding:.8em 1em;font-size:110%' }, [
		dot('#2e9e44'), E('strong', {}, _('Running')), ' · ',
		_('%d device(s) receiving %d stream(s)').format(hosts.length, tv.length), ' · ',
		_('streams stop %s').format(stopAfter),
		E('div', { 'style': 'font-size:85%;opacity:.7;margin-top:.3em' },
			_('Fast leave: %s · IGMPv%d queries · up %s').format(c.fastleave, c.query_version || 2, dur(st.uptime_ms)))
	]);

	var watchRows = hosts.map(function(ip) {
		var h = byHost[ip];
		return [
			device(ip, names),
			E('span', {}, h.channels.map(function(ch) { return E('code', { 'style': 'margin-right:.5em' }, ch); })),
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
			E('code', {}, g.group),
			E('span', {}, channelState(g)),
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
		E('h3', {}, _('Devices')),
		table([_('Device'), _('Streams'), _('IGMP')], watchRows, _('No device is receiving a stream right now')),
		E('h3', {}, _('Streams')),
		table([_('Stream (group)'), _('State'), _('Receiving'), _('Running for')], chanRows, _('No streams right now')),
		E('details', { 'style': 'margin-top:1.5em' }, [
			E('summary', { 'style': 'cursor:pointer' }, _('Details: interfaces and other multicast')),
			table([_('Interface'), _('Address'), _('Role'), _('Leave settings')], ifRows, _('No interfaces')),
			otherRows.length ? table([_('Group'), _('What it is'), _('Devices')], otherRows, '') : ''
		])
	]);
}

return view.extend({
	load: function() {
		return Promise.all([fetchStatus(), fetchNames()]);
	},

	render: function(data) {
		var names = data[1];
		var container = E('div', { 'id': 'igmpproxy-status' }, render(data[0], names));

		poll.add(function() {
			return fetchStatus().then(function(st) {
				var c = document.getElementById('igmpproxy-status');
				if (!c)
					return;
				// keep the details section open across refreshes
				var d = c.querySelector('details');
				var open = d && d.open;
				c.replaceChildren(render(st, names));
				if (open)
					c.querySelector('details').open = true;
			});
		}, 2);

		return E('div', { 'class': 'cbi-map' }, [
			E('h2', {}, _('IGMP Proxy')),
			E('div', { 'class': 'cbi-map-descr' }, _('Which devices (IPTV boxes, speakers, computers...) receive which multicast streams from your provider, live.')),
			container
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});

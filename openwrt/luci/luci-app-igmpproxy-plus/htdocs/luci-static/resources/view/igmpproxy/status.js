'use strict';
'require view';
'require fs';
'require poll';
'require ui';

function fetchStatus() {
	return fs.exec('/usr/sbin/igmpproxy', ['-S']).then(function(res) {
		if (res.code !== 0)
			return { error: (res.stderr || '').trim() || _('igmpproxy is not running') };
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

function ms(v) {
	if (v == null || v < 0)
		return '-';
	if (v < 10000)
		return '%.1f s'.format(v / 1000);
	return '%t'.format(Math.round(v / 1000));
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

function renderStatus(st) {
	if (st.error)
		return E('div', { 'class': 'alert-message warning' }, st.error);

	var c = st.config;
	var summary = table(
		[_('Fast leave'), _('Last member queries'), _('Upstream leave delay'), _('Query interval'), _('Robustness'), _('Uptime')],
		[[
			c.fastleave,
			'%d x %d ms'.format(c.last_member_query_count, c.last_member_query_interval_ms),
			'%d ms'.format(c.upstream_leave_delay_ms),
			ms(c.query_interval_ms),
			String(c.robustness),
			'%t'.format(Math.round(st.uptime_ms / 1000))
		]], '');

	var ifRows = st.interfaces.map(function(i) {
		return [
			i.name, i.address, i.state,
			i.state == 'downstream' ? i.fastleave : '-',
			i.state == 'downstream' ? '%d x %d ms'.format(i.last_member_query_count, i.last_member_query_interval_ms) : '-',
			i.state == 'downstream' ? ms(i.group_membership_interval_ms) : '-'
		];
	});

	var groupRows = [];
	st.groups.forEach(function(g) {
		var members = g.members.map(function(m) {
			var hosts = m.hosts.map(function(h) { return h.address; }).join(', ');
			if (m.hosts_untracked)
				hosts += (hosts ? ', ' : '') + _('(more, untracked)');
			var state = m.forwarding
				? (m.last_member_queries_left > 0 ? _('checking (%d queries left)').format(m.last_member_queries_left) : _('forwarding'))
				: _('stopped, querying');
			return E('div', {}, [
				E('strong', {}, m.interface), ' ', state, ' – ', _('expires %s').format(ms(m.expires_in_ms)),
				hosts ? E('div', { 'style': 'opacity:.75' }, hosts) : ''
			]);
		});

		groupRows.push([
			E('strong', {}, g.group),
			g.sources.join(', ') || '-',
			g.upstream == 'joined'
				? (g.leave_in_ms >= 0 ? _('leaving in %s').format(ms(g.leave_in_ms)) : _('joined'))
				: _('not joined'),
			members.length ? E('div', {}, members) : E('em', {}, _('no listeners')),
			'%d / %d'.format(g.reports, g.leaves),
			ms(g.age_ms)
		]);
	});

	return E('div', {}, [
		E('h3', {}, _('Settings in use')), summary,
		E('h3', {}, _('Interfaces')),
		table([_('Interface'), _('Address'), _('Role'), _('Fast leave'), _('Last member queries'), _('Membership timeout')],
			ifRows, _('No interfaces')),
		E('h3', {}, _('Multicast groups')),
		table([_('Group'), _('Sources'), _('Upstream'), _('Downstream members'), _('Reports / leaves'), _('Age')],
			groupRows, _('No active groups'))
	]);
}

return view.extend({
	load: function() {
		return fetchStatus();
	},

	render: function(st) {
		var container = E('div', { 'id': 'igmpproxy-status' }, renderStatus(st));

		poll.add(function() {
			return fetchStatus().then(function(s) {
				var c = document.getElementById('igmpproxy-status');
				if (c)
					c.replaceChildren(renderStatus(s));
			});
		}, 2);

		return E('div', { 'class': 'cbi-map' }, [
			E('h2', {}, _('IGMP Proxy – Status')),
			E('div', { 'class': 'cbi-map-descr' },
				_('Live group table from the running proxy. Refreshes every 2 seconds.')),
			container
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});

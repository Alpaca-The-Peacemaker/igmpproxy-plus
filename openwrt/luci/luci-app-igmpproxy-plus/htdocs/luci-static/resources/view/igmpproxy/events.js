'use strict';
'require view';
'require ui';
'require igmpproxy.common as common';

var SHOW = 500;       /* rows rendered at most; the download has everything */
var ISSUE_HOURS = 24;

function secs(ms) { return (ms / 1000).toFixed(1) + ' s'; }

/* plain-language description of one event */
function describe(e, names) {
	var dev = e.host ? (names[e.host] || e.host) : '';
	switch (e.ev) {
	case 'start':
		return _('Proxy started (%s, fast leave %s, IGMPv%d queries)').format(e.version, e.fastleave, e.query_version);
	case 'exit':
		return _('Proxy stopped');
	case 'join':
		return _('%s joined (IGMPv%d)').format(dev, e.igmp);
	case 'report':
		return _('%s is still receiving').format(dev);
	case 'leave':
		switch (e.action) {
		case 'keeps': return _('%s left; still received by %d other device(s)').format(dev, e.others);
		case 'checking': return _('%s left; checking whether another device still receives it (%s)').format(dev, secs(e.check_ms));
		case 'stopped': return _('%s left; stream stopped at once (fast leave %s)').format(dev, e.mode);
		default: return _('%s left (the stream was not running here)').format(dev);
		}
	case 'leave_answered':
		return _('%s answered: still receiving after %s left').format(dev, names[e.left] || e.left);
	case 'forward_start':
		return _('Stream started (requested by %s)').format(dev);
	case 'forward_stop':
		switch (e.reason) {
		case 'no_answer': return _('Stream stopped: no device answered after %s left').format(dev);
		case 'timeout': return _('Stream stopped: no device reported for %d s').format(e.timeout_s);
		default: return _('Stream stopped (fast leave)');
		}
	case 'host_timeout':
		return _('%s stopped answering (timed out)').format(dev);
	case 'compat_v2':
		return _('%s uses IGMPv%d: IGMPv2 compatibility on').format(dev, e.igmp);
	case 'upstream_join':
		return _('Joined the stream from the provider');
	case 'first_packet':
		return _('First data from the provider arrived after %s').format(secs(e.delay_ms));
	case 'upstream_leave':
		return _('Left the stream at the provider');
	case 'upstream_join_failed':
		return _('Could not request the stream from the provider (%s); retrying').format(e.error);
	case 'query':
		if (e.kind == 'group')
			return _('Asked who still receives this stream (IGMPv%d)').format(e.igmp);
		return _('Asked all devices what they receive (%s)').format(e.why);
	}
	return e.ev;
}

var IMPORTANT = { start: 1, exit: 1, join: 1, leave: 1, leave_answered: 1, forward_start: 1,
	forward_stop: 1, host_timeout: 1, compat_v2: 1, upstream_join: 1, first_packet: 1, upstream_leave: 1, upstream_join_failed: 1 };

function color(e) {
	if (e.ev == 'forward_stop' || e.ev == 'host_timeout' || e.ev == 'upstream_join_failed') return '#c0392b';
	if (e.ev == 'leave') return '#e0a000';
	if (e.ev == 'join' || e.ev == 'forward_start' || e.ev == 'leave_answered' || e.ev == 'first_packet') return '#2e9e44';
	return '#888';
}

return view.extend({
	load: function() {
		return Promise.all([common.fetchEvents(), common.fetchNames(), common.loadChannels()]);
	},

	render: function(data) {
		var self = this;
		this.data = data[0];
		this.names = data[1];
		this.channels = data[2];
		this.issues = E('div');

		this.filter = E('input', { 'type': 'text', 'class': 'cbi-input-text', 'style': 'width:16em',
			'placeholder': _('Filter: device, group, event...'), 'input': function() { self.update(); } });
		this.routine = E('input', { 'type': 'checkbox', 'change': function() { self.update(); } });
		this.body = E('div');

		var toolbar = E('div', { 'style': 'display:flex;flex-wrap:wrap;gap:.6em;align-items:center;margin-bottom:1em' }, [
			this.filter,
			E('label', { 'style': 'white-space:nowrap' }, [ this.routine, ' ', _('Show routine reports and queries') ]),
			E('button', { 'class': 'cbi-button', 'click': ui.createHandlerFn(this, 'reload') }, _('Reload')),
			E('button', { 'class': 'cbi-button cbi-button-action', 'click': ui.createHandlerFn(this, 'download') }, _('Download log'))
		]);

		this.update();

		return E('div', { 'class': 'cbi-map' }, [
			E('h2', {}, _('IGMP Proxy: Events')),
			E('div', { 'class': 'cbi-map-descr' },
				_('History of joins, leaves, stream starts and stops, timeouts and queries, newest first. The download is the complete log as JSON Lines (one event per line), ready for scripts or an AI assistant ("why did the stream on 192.168.1.84 stop at 21:04?").')),
			this.issues,
			toolbar,
			this.body
		]);
	},

	update: function() {
		var d = this.data, names = this.names, channels = this.channels;
		if (d.error) {
			this.issues.replaceChildren();
			this.body.replaceChildren(E('div', { 'class': 'alert-message warning' }, d.error));
			return;
		}
		this.issues.replaceChildren(common.issueBox(common.analyze(d.events, names, channels, ISSUE_HOURS), ISSUE_HOURS));
		var q = (this.filter.value || '').toLowerCase(), routine = this.routine.checked;
		var rows = [];
		for (var i = d.events.length - 1; i >= 0 && rows.length < SHOW; i--) {
			var e = d.events[i];
			if (!routine && !IMPORTANT[e.ev])
				continue;
			var text = describe(e, names);
			if (q && (text + ' ' + JSON.stringify(e) + ' ' + (names[e.host] || '') + ' ' + (channels[e.group] || '')).toLowerCase().indexOf(q) < 0)
				continue;
			rows.push(E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td', 'style': 'white-space:nowrap' }, common.localTime(e.ts)),
				E('td', { 'class': 'td' }, [ E('span', { 'style': 'color:' + color(e) + ';margin-right:.35em' }, '●'), text ]),
				E('td', { 'class': 'td' }, e.group ? common.channel(e.group, channels) : ''),
				E('td', { 'class': 'td' }, e['if'] || '')
			]));
		}
		var first = d.events.length ? common.localTime(d.events[0].ts) : '-';
		var table = E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, _('Time')),
				E('th', { 'class': 'th' }, _('What happened')),
				E('th', { 'class': 'th' }, _('Stream')),
				E('th', { 'class': 'th' }, _('Interface'))
			])
		]);
		if (!rows.length)
			table.appendChild(E('tr', { 'class': 'tr placeholder' },
				E('td', { 'class': 'td', 'colspan': 4 }, E('em', {}, _('No matching events')))));
		rows.forEach(function(r) { table.appendChild(r); });
		this.body.replaceChildren(
			E('p', { 'style': 'opacity:.7' }, _('%d events since %s%s').format(d.events.length, first,
				rows.length >= SHOW ? _(', showing the newest %d').format(SHOW) : '')),
			table);
	},

	reload: function() {
		var self = this;
		return common.fetchEvents().then(function(d) {
			self.data = d;
			self.update();
		});
	},

	download: function() {
		var raw = this.data.raw || '';
		var stamp = new Date().toISOString().replace(/[:T]/g, '-').slice(0, 16);
		var a = E('a', {
			'href': URL.createObjectURL(new Blob([ raw ], { 'type': 'application/x-ndjson' })),
			'download': 'igmpproxy-events-' + stamp + '.jsonl',
			'style': 'display:none'
		});
		document.body.appendChild(a);
		a.click();
		document.body.removeChild(a);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});

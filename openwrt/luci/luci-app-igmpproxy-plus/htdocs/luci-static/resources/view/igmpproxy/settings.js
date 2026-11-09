'use strict';
'require view';
'require form';
'require uci';
'require tools.widgets as widgets';
'require ui';
'require igmpproxy.common as common';

var fastLeaveHelp = _('<b>A stream never stops while another device that asked for it is still receiving it</b>, in every mode. The modes only decide how fast it stops once the last device has left. <b>Off</b>: after the router checks that nobody else is still receiving (count x interval). <b>On</b>: instantly. <b>Auto</b>: instantly with IGMPv3 devices, after the check with IGMPv2 devices.');

function validateRange(min, max, unit) {
	return function(section_id, value) {
		if (value == null || value === '')
			return true;
		var n = +value;
		if (isNaN(n) || n < min || n > max)
			return _('Must be between %s and %s %s').format(min, max, unit);
		return true;
	};
}

/* Channel names: a list the user loads or pastes, kept in /etc. */
function channelSection(text) {
	var ta = E('textarea', {
		'class': 'cbi-input-textarea', 'wrap': 'off', 'spellcheck': 'false',
		'style': 'width:100%;min-height:12em;font-family:monospace',
		'placeholder': '#EXTM3U\n#EXTINF:-1,RTP 1\nrtp://@239.1.1.1:5000\n\n' + _('or one per line:') + '\n239.1.1.2 SIC'
	}, [ text ]);
	var info = E('span', { 'style': 'opacity:.75;margin-left:1em' });
	var count = function() {
		info.textContent = _('%d channel name(s) recognised').format(Object.keys(common.parseChannels(ta.value)).length);
	};
	ta.addEventListener('input', count);
	count();

	var file = E('input', { 'type': 'file', 'accept': '.m3u,.m3u8,.txt,.csv,text/plain', 'change': function(ev) {
		var f = ev.target.files[0];
		if (!f)
			return;
		var r = new FileReader();
		r.onload = function() { ta.value = r.result; count(); };
		r.readAsText(f);
	} });

	var save = E('button', { 'class': 'cbi-button cbi-button-save', 'click': ui.createHandlerFn(this, function() {
		return common.saveChannels(ta.value).then(function() {
			ui.addNotification(null, E('p', _('Channel names saved: %d.').format(Object.keys(common.parseChannels(ta.value)).length)), 'info');
		}).catch(function(e) {
			ui.addNotification(null, E('p', _('Could not save the channel names: %s').format(e.message)), 'danger');
		});
	}) }, _('Save channel names'));

	return E('div', { 'class': 'cbi-section' }, [
		E('h3', {}, _('Channel names')),
		E('div', { 'class': 'cbi-section-descr' }, _('Load or paste your provider\'s channel list to see channel names instead of multicast addresses on the Status and Events pages. Accepted: M3U/M3U8 playlists (rtp://, udp:// or udpxy http://.../rtp/ links) and plain lines such as "239.1.1.1 RTP 1" or "RTP 1,239.1.1.1". Lists for most providers are maintained by users in forums; this package does not ship any. Stored in %s and kept across upgrades. This button saves only the list; the settings above have their own Save.').format(common.CHANNELS)),
		E('div', { 'style': 'margin:.5em 0' }, [ file ]),
		ta,
		E('div', { 'style': 'margin-top:.5em' }, [ save, info ])
	]);
}

return view.extend({
	load: function() {
		return Promise.all([uci.load('igmpproxy'), common.loadChannelText()]);
	},

	render: function(data) {
		var m, s, o, channelText = data[1] || '';

		m = new form.Map('igmpproxy', _('IGMP Proxy'),
			_('Multicast proxy with fine grained control over fast leave and the timers that decide how quickly multicast traffic stops after a leave.'));

		/* ---- Global ------------------------------------------------- */
		s = m.section(form.TypedSection, 'igmpproxy', _('General settings'));
		s.anonymous = true;
		s.addremove = false;

		s.tab('leave', _('Leave behaviour'));
		s.tab('querier', _('Querier timers'));
		s.tab('advanced', _('Advanced'));

		o = s.taboption('leave', form.ListValue, 'fastleave', _('Fast leave'), fastLeaveHelp);
		o.value('off', _('Off (RFC 2236)'));
		o.value('on', _('On (immediate)'));
		o.value('auto', _('Auto (last tracked host)'));
		o.default = 'off';
		o.cfgvalue = function(section_id) {
			var v = uci.get('igmpproxy', section_id, 'fastleave');
			if (!v && uci.get('igmpproxy', section_id, 'quickleave') == '1')
				return 'auto';
			return v || 'off';
		};
		o.write = function(section_id, value) {
			uci.unset('igmpproxy', section_id, 'quickleave');
			return uci.set('igmpproxy', section_id, 'fastleave', value);
		};

		o = s.taboption('leave', form.Value, 'last_member_interval', _('Last member query interval'),
			_('Milliseconds between group specific queries after a leave. Stock igmpproxy used 10000 ms. RFC default 1000 ms.'));
		o.datatype = 'uinteger';
		o.placeholder = '1000';
		o.validate = validateRange(100, 25500, 'ms');

		o = s.taboption('leave', form.Value, 'last_member_count', _('Last member query count'),
			_('Queries sent before forwarding stops. Time to stop = count x interval. Defaults to the robustness value.'));
		o.datatype = 'range(1,10)';
		o.placeholder = '2';

		o = s.taboption('leave', form.Value, 'upstream_leave_delay', _('Upstream leave delay'),
			_('Milliseconds to stay joined upstream after the last downstream member is gone. Switching back to a channel inside this window is instant. 0 = leave immediately.'));
		o.datatype = 'uinteger';
		o.placeholder = '0';
		o.validate = validateRange(0, 300000, 'ms');

		o = s.taboption('leave', form.ListValue, 'upstream_igmp_version', _('Upstream IGMP version'),
			_('Force the IGMP version used for joins and leaves on the upstream interface. Some IPTV providers need IGMPv2 to see leaves quickly.'));
		o.value('', _('Automatic'));
		o.value('2', 'IGMPv2');
		o.value('3', 'IGMPv3');

		o = s.taboption('querier', form.ListValue, 'query_version', _('Query version'),
			_('IGMPv3 queries keep hosts in IGMPv3, where every host reports and sends its leave, so host tracking and auto fast leave are exact. IGMPv2 hosts still work. Choose IGMPv2 only for old equipment that needs it.'));
		o.value('3', _('IGMPv3'));
		o.value('2', _('IGMPv2'));
		o.default = '3';

		o = s.taboption('querier', form.Value, 'robustness', _('Robustness'),
			_('Tolerance for lost packets. Membership timeout = robustness x query interval + response interval.'));
		o.datatype = 'range(1,7)';
		o.placeholder = '2';

		o = s.taboption('querier', form.Value, 'query_interval', _('Query interval'),
			_('Seconds between general queries on downstream interfaces.'));
		o.datatype = 'range(2,3600)';
		o.placeholder = '125';

		o = s.taboption('querier', form.Value, 'query_response_interval', _('Query response interval'),
			_('Seconds hosts may wait before answering a general query (max 25.5).'));
		o.datatype = 'ufloat';
		o.placeholder = '10';
		o.validate = validateRange(1, 25.5, 's');

		o = s.taboption('querier', form.Value, 'startup_query_interval', _('Startup query interval'),
			_('Seconds between the general queries sent at startup. Default: query interval / 4.'));
		o.datatype = 'ufloat';
		o.placeholder = '31.25';

		o = s.taboption('querier', form.Value, 'startup_query_count', _('Startup query count'),
			_('Default: robustness.'));
		o.datatype = 'range(0,10)';
		o.placeholder = '2';

		o = s.taboption('advanced', form.Value, 'host_limit', _('Tracked hosts limit'),
			_('Hosts tracked per group per interface. Above it, auto fast leave falls back to the RFC check.'));
		o.datatype = 'range(0,65535)';
		o.placeholder = '256';

		o = s.taboption('advanced', form.Flag, 'proxy_ssdp', _('Proxy SSDP (239.255.255.250)'),
			_('By default UPnP discovery traffic is kept local and never joined upstream. Enable only if you really need SSDP from the upstream network.'));
		o.default = '0';

		o = s.taboption('advanced', form.Flag, 'event_log', _('Event log'),
			_('Keep a history of joins, leaves, stream starts and stops, timeouts and queries (Services > IGMP Proxy > Events). One JSON object per line, easy to analyse with scripts or an AI assistant.'));
		o.default = '1';

		o = s.taboption('advanced', form.Value, 'event_log_path', _('Event log file'),
			_('/tmp is RAM: cleared on reboot, never wears the flash. Point it to a USB stick to keep it across reboots.'));
		o.placeholder = '/tmp/igmpproxy-events.jsonl';
		o.depends('event_log', '1');

		o = s.taboption('advanced', form.Value, 'event_log_size', _('Event log size (KiB)'),
			_('When the file reaches this size it is renamed to .1 and a new one starts, so at most twice this is kept. 1024 KiB holds about one to two days on a typical home network.'));
		o.datatype = 'range(16,65536)';
		o.placeholder = '1024';
		o.depends('event_log', '1');

		o = s.taboption('advanced', form.Flag, 'event_log_reports', _('Log periodic reports and queries'),
			_('Also log the routine "still receiving" reports and the periodic queries. Shows exactly when each device last answered; turn off for a shorter log.'));
		o.default = '1';
		o.depends('event_log', '1');

		o = s.taboption('advanced', form.ListValue, 'verbose', _('Log level'));
		o.value('1', _('Warnings'));
		o.value('2', _('Info (joins, leaves, fast leave)'));
		o.value('3', _('Debug'));
		o.default = '1';

		/* ---- Interfaces --------------------------------------------- */
		s = m.section(form.GridSection, 'phyint', _('Interfaces'),
			_('One upstream interface (towards the multicast source) and one or more downstream interfaces. Per interface leave settings override the general ones.'));
		s.anonymous = true;
		s.addremove = true;
		s.sortable = true;
		s.nodescriptions = true;

		s.tab('general', _('General'));
		s.tab('leave', _('Leave behaviour'));
		s.tab('filters', _('Filters'));

		o = s.taboption('general', form.Flag, 'enabled', _('Enabled'));
		o.default = '1';
		o.editable = true;

		o = s.taboption('general', widgets.NetworkSelect, 'network', _('Network'));
		o.nocreate = true;
		o.rmempty = false;

		o = s.taboption('general', form.ListValue, 'direction', _('Direction'));
		o.value('upstream', _('Upstream'));
		o.value('downstream', _('Downstream'));
		o.rmempty = false;

		o = s.taboption('general', widgets.ZoneSelect, 'zone', _('Firewall zone'),
			_('Adds the IGMP and multicast forwarding firewall rules for this zone.'));
		o.nocreate = true;

		o = s.taboption('general', form.Value, 'threshold', _('TTL threshold'));
		o.datatype = 'range(1,255)';
		o.placeholder = '1';
		o.modalonly = true;

		o = s.taboption('leave', form.ListValue, 'fastleave', _('Fast leave'), fastLeaveHelp);
		o.value('', _('Use general setting'));
		o.value('off', _('Off (RFC 2236)'));
		o.value('on', _('On (immediate)'));
		o.value('auto', _('Auto (last tracked host)'));
		o.depends('direction', 'downstream');
		o.textvalue = function(section_id) {
			if (uci.get('igmpproxy', section_id, 'direction') != 'downstream')
				return '-';
			return this.cfgvalue(section_id) || _('general');
		};

		o = s.taboption('leave', form.Value, 'last_member_interval', _('Last member query interval (ms)'));
		o.datatype = 'uinteger';
		o.placeholder = _('general');
		o.validate = validateRange(100, 25500, 'ms');
		o.depends('direction', 'downstream');
		o.modalonly = true;

		o = s.taboption('leave', form.Value, 'last_member_count', _('Last member query count'));
		o.datatype = 'range(1,10)';
		o.placeholder = _('general');
		o.depends('direction', 'downstream');
		o.modalonly = true;

		o = s.taboption('leave', form.Value, 'robustness', _('Robustness'));
		o.datatype = 'range(1,7)';
		o.placeholder = _('general');
		o.depends('direction', 'downstream');
		o.modalonly = true;

		o = s.taboption('leave', form.ListValue, 'query_version', _('Query version'));
		o.value('', _('Use general setting'));
		o.value('3', _('IGMPv3'));
		o.value('2', _('IGMPv2'));
		o.depends('direction', 'downstream');
		o.modalonly = true;

		o = s.taboption('leave', form.Value, 'group_timeout', _('Group membership timeout (s)'),
			_('How long a group stays forwarded without a report. Empty = robustness x query interval + response interval.'));
		o.datatype = 'range(1,7200)';
		o.placeholder = _('computed');
		o.depends('direction', 'downstream');
		o.modalonly = true;

		o = s.taboption('filters', form.DynamicList, 'altnet', _('Allowed source networks'),
			_('Upstream: networks multicast sources may come from (e.g. 0.0.0.0/0).'));
		o.datatype = 'cidr4';
		o.modalonly = true;

		o = s.taboption('filters', form.DynamicList, 'whitelist', _('Group whitelist'),
			_('Only these groups are proxied on this interface.'));
		o.datatype = 'cidr4';
		o.modalonly = true;

		o = s.taboption('filters', form.DynamicList, 'blacklist', _('Group blacklist'),
			_('These groups are never proxied on this interface (e.g. 239.255.255.250/32 for SSDP).'));
		o.datatype = 'cidr4';
		o.modalonly = true;

		return m.render().then(function(node) {
			node.appendChild(channelSection(channelText));
			return node;
		});
	}
});

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

	var allow = E('button', { 'class': 'cbi-button', 'style': 'margin-left:.5em',
		'title': _('Set the group whitelist of the upstream interface to the networks of this channel list'),
		'click': ui.createHandlerFn(this, function() {
			var nets = common.whitelistFromChannels(common.parseChannels(ta.value));
			var ups = uci.sections('igmpproxy', 'phyint').filter(function(s) { return s.direction == 'upstream'; });
			if (!nets.length || !ups.length) {
				ui.addNotification(null, E('p', !nets.length ? _('The channel list is empty.') : _('There is no upstream interface.')), 'warning');
				return;
			}
			if (!confirm(_('Only groups in these networks will be requested from the provider:\n\n%s\n\nEverything else (smart home discovery and so on) stays local. Careful: set-top boxes may also need groups that are not in a channel list (programme guide, software updates). If something stops working, remove the whitelist under Interfaces > Edit > Filters.\n\nContinue?').format(nets.join('\n'))))
				return;
			ups.forEach(function(s) { uci.set('igmpproxy', s['.name'], 'whitelist', nets); });
			return uci.save().then(function() {
				ui.changes.init();
				ui.addNotification(null, E('p', _('Whitelist of %d network(s) prepared for the upstream interface. Press "Save & Apply" to activate it.').format(nets.length)), 'info');
			});
		}) }, _('Allow only these channels upstream'));

	return E('div', { 'class': 'cbi-section' }, [
		E('h3', {}, _('Channel names')),
		E('div', { 'class': 'cbi-section-descr' }, _('Load or paste your provider\'s channel list to see channel names instead of multicast addresses on the Status and Events pages. Accepted: M3U/M3U8 playlists (rtp://, udp:// or udpxy http://.../rtp/ links) and plain lines such as "239.1.1.1 RTP 1" or "RTP 1,239.1.1.1". Lists for most providers are maintained by users in forums; this package does not ship any. Stored in %s and kept across upgrades. This button saves only the list; the settings above have their own Save.').format(common.CHANNELS)),
		E('div', { 'style': 'margin:.5em 0' }, [ file ]),
		ta,
		E('div', { 'style': 'margin-top:.5em' }, [ save, allow, info ])
	]);
}

/*
 * "Add a TV network": a second upstream for providers that deliver TV on
 * their own VLAN. Only stages UCI changes; nothing is applied until
 * Save & Apply.
 */
function tvNetworkSection(chanNets) {
	var open = function() {
		var used = uci.sections('igmpproxy', 'phyint').map(function(p) { return p.network; });
		var nets = uci.sections('network', 'interface').map(function(n) { return n['.name']; })
			.filter(function(n) { return n != 'loopback' && used.indexOf(n) < 0; });
		var zones = uci.sections('firewall', 'zone');
		var zoneOf = function(net) {
			var z = zones.filter(function(z) { return L.toArray(z.network).indexOf(net) >= 0; })[0];
			return z ? z.name : '';
		};

		if (!nets.length) {
			ui.addNotification(null, E('p', _('Every network is already used by the IGMP proxy. Create the TV VLAN interface first (Network > Interfaces).')), 'warning');
			return;
		}

		var netSel = E('select', { 'class': 'cbi-input-select' }, nets.map(function(n) { return E('option', { 'value': n }, n); }));
		var zoneSel = E('select', { 'class': 'cbi-input-select' }, [ E('option', { 'value': '' }, _('none')) ].concat(
			zones.map(function(z) { return E('option', { 'value': z.name }, z.name); })));
		var syncZone = function() { zoneSel.value = zoneOf(netSel.value); };
		netSel.addEventListener('change', syncZone);
		syncZone();
		var srcIn = E('input', { 'class': 'cbi-input-text', 'type': 'text', 'value': '0.0.0.0/0', 'style': 'width:100%' });
		var onlyCh = E('input', { 'type': 'checkbox', 'disabled': chanNets.length ? null : '' });
		var notOthers = E('input', { 'type': 'checkbox', 'disabled': chanNets.length ? null : '' });

		ui.showModal(_('Add a TV network'), [
			E('p', {}, _('For providers that deliver TV on its own VLAN or interface, next to the internet one. The proxy then requests streams on both upstream interfaces, each limited by its own filters.')),
			E('div', { 'class': 'cbi-value' }, [ E('label', { 'class': 'cbi-value-title' }, _('TV network')), E('div', { 'class': 'cbi-value-field' }, netSel) ]),
			E('div', { 'class': 'cbi-value' }, [ E('label', { 'class': 'cbi-value-title' }, _('Firewall zone')), E('div', { 'class': 'cbi-value-field' }, zoneSel) ]),
			E('div', { 'class': 'cbi-value' }, [ E('label', { 'class': 'cbi-value-title' }, _('Stream sources')), E('div', { 'class': 'cbi-value-field' }, [ srcIn,
				E('div', { 'class': 'cbi-value-description' }, _('Networks the TV streams come from, separated by spaces. Ask your provider or use 0.0.0.0/0 (anywhere).')) ]) ]),
			E('div', { 'class': 'cbi-value' }, [ E('label', { 'class': 'cbi-value-title' }, _('Channels')), E('div', { 'class': 'cbi-value-field' }, [
				E('label', {}, [ onlyCh, ' ', _('Request only the channels of my channel list on this network') ]), E('br'),
				E('label', {}, [ notOthers, ' ', _('Do not request these channels on the other upstream interfaces') ]),
				chanNets.length ? '' : E('div', { 'class': 'cbi-value-description' }, _('Load a channel list under Channel names to use these options.')) ]) ]),
			E('div', { 'class': 'right' }, [
				E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ',
				E('button', { 'class': 'btn cbi-button-action', 'click': ui.createHandlerFn(this, function() {
					var src = (srcIn.value || '').split(/[\s,]+/).filter(function(x) { return x; });
					if (!src.length || src.some(function(x) { return !/^\d{1,3}(\.\d{1,3}){3}\/\d{1,2}$/.test(x); })) {
						ui.addNotification(null, E('p', _('Stream sources must be networks such as 10.0.0.0/8 or 0.0.0.0/0.')), 'warning');
						return;
					}
					var sid = uci.add('igmpproxy', 'phyint');
					uci.set('igmpproxy', sid, 'network', netSel.value);
					uci.set('igmpproxy', sid, 'direction', 'upstream');
					uci.set('igmpproxy', sid, 'altnet', src);
					if (zoneSel.value)
						uci.set('igmpproxy', sid, 'zone', zoneSel.value);
					if (onlyCh.checked)
						uci.set('igmpproxy', sid, 'whitelist', chanNets);
					if (notOthers.checked)
						uci.sections('igmpproxy', 'phyint').forEach(function(p) {
							if (p['.name'] == sid || p.direction != 'upstream')
								return;
							var bl = L.toArray(p.blacklist);
							chanNets.forEach(function(n) { if (bl.indexOf(n) < 0) bl.push(n); });
							uci.set('igmpproxy', p['.name'], 'blacklist', bl);
						});
					return uci.save().then(function() {
						ui.hideModal();
						window.location.reload();
					});
				}) }, _('Add'))
			])
		]);
	};

	return E('div', { 'class': 'cbi-section' }, [
		E('h3', {}, _('TV on its own network')),
		E('div', { 'class': 'cbi-section-descr' }, _('Some providers deliver TV on a separate VLAN or interface. This adds it as a second upstream interface; nothing changes until you press Save & Apply.')),
		E('button', { 'class': 'cbi-button cbi-button-add', 'click': ui.createHandlerFn(this, open) }, _('Add a TV network…'))
	]);
}

return view.extend({
	load: function() {
		return Promise.all([uci.load('igmpproxy'), common.loadChannelText(),
			L.resolveDefault(uci.load('network'), null), L.resolveDefault(uci.load('firewall'), null)]);
	},

	render: function(data) {
		var m, s, o, channelText = data[1] || '';
		var chanNets = common.whitelistFromChannels(common.parseChannels(channelText));

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

		o = s.taboption('advanced', form.Flag, 'proxy_ssdp', _('Proxy local discovery (SSDP, CoAP)'),
			_('By default UPnP (239.255.255.250) and smart home (224.0.1.187) discovery traffic is kept local and never joined upstream. Enable only if you really need it from the upstream network.'));
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

		o = s.taboption('advanced', form.Flag, 'querier_election', _('Querier election'),
			_('Off (default): this router always sends the queries. On: if another router on the same network sends queries and has a lower address, this router lets it be the querier and only listens (RFC 2236/3376). It still checks by itself whether anyone wants a stream after a leave, so streams stop as quickly as before. Turn it on when another multicast router or a switch with an active querier shares the network.'));
		o.default = '0';

		o = s.taboption('advanced', form.Value, 'stall_timeout', _('Stalled stream alert (s)'),
			_('A stream that devices receive and that carried data (at least 500 kbit/s) but has had none for this long is reported as stalled on the Status and Events pages. 0 = off.'));
		o.datatype = 'range(0,3600)';
		o.placeholder = '10';

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
		o.value('querier', _('Querier only (no forwarding)'));
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
		o.depends('direction', 'querier');
		o.modalonly = true;

		o = s.taboption('leave', form.Value, 'last_member_count', _('Last member query count'));
		o.datatype = 'range(1,10)';
		o.placeholder = _('general');
		o.depends('direction', 'downstream');
		o.depends('direction', 'querier');
		o.modalonly = true;

		o = s.taboption('leave', form.Value, 'robustness', _('Robustness'));
		o.datatype = 'range(1,7)';
		o.placeholder = _('general');
		o.depends('direction', 'downstream');
		o.depends('direction', 'querier');
		o.modalonly = true;

		o = s.taboption('leave', form.ListValue, 'query_version', _('Query version'));
		o.value('', _('Use general setting'));
		o.value('3', _('IGMPv3'));
		o.value('2', _('IGMPv2'));
		o.depends('direction', 'downstream');
		o.depends('direction', 'querier');
		o.modalonly = true;

		o = s.taboption('leave', form.ListValue, 'querier_election', _('Querier election'),
			_('Whether this router gives way to another querier with a lower address on this network.'));
		o.value('', _('Use general setting'));
		o.value('1', _('On'));
		o.value('0', _('Off (always query)'));
		o.depends('direction', 'downstream');
		o.depends('direction', 'querier');
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

		/* simple choice over the options above (declared last, so it wins) */
		o = s.taboption('general', form.ListValue, '_policy', _('What this network may request'),
			_('Only the channel list: the provider is only asked for streams in the networks of your channel list (Settings > Channel names); set-top boxes may also need groups outside it (programme guide, updates), so check the TV afterwards. Nothing: this network only gets queries so its switches deliver multicast to the right ports (querier only, nothing is forwarded). Custom: the whitelist and blacklist on the Filters tab.'));
		o.value('all', _('Everything (no filter)'));
		if (chanNets.length)
			o.value('channels', _('Only the channels of my channel list'));
		o.value('none', _('Nothing from the provider (querier only)'));
		o.value('custom', _('Custom (Filters tab)'));
		o.depends('direction', 'downstream');
		o.depends('direction', 'querier');
		o.cfgvalue = function(sid) {
			return common.policyOf(uci.get('igmpproxy', sid, 'direction'), uci.get('igmpproxy', sid, 'whitelist'),
				uci.get('igmpproxy', sid, 'blacklist'), chanNets);
		};
		o.textvalue = function(sid) {
			var d = uci.get('igmpproxy', sid, 'direction');
			if (d != 'downstream' && d != 'querier')
				return '-';
			return { all: _('everything'), channels: _('channel list'), none: _('nothing (querier)'), custom: _('custom') }[this.cfgvalue(sid)];
		};
		o.write = function(sid, v) {
			if (v == 'none') {
				uci.set('igmpproxy', sid, 'direction', 'querier');
				return;
			}
			if (uci.get('igmpproxy', sid, 'direction') == 'querier')
				uci.set('igmpproxy', sid, 'direction', 'downstream');
			if (v == 'all') {
				uci.unset('igmpproxy', sid, 'whitelist');
				uci.unset('igmpproxy', sid, 'blacklist');
			}
			else if (v == 'channels') {
				uci.set('igmpproxy', sid, 'whitelist', chanNets);
				uci.unset('igmpproxy', sid, 'blacklist');
			}
		};
		o.remove = function() {};

		return m.render().then(function(node) {
			node.appendChild(tvNetworkSection(chanNets));
			node.appendChild(channelSection(channelText));
			return node;
		});
	}
});

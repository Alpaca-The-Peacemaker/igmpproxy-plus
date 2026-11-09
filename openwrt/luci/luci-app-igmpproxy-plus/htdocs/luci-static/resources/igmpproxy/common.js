'use strict';
'require baseclass';
'require fs';
'require rpc';

/*
 * Shared by the Status, Events and Settings pages of luci-app-igmpproxy-plus:
 * channel names, device names, the event log and its automatic analysis.
 */

var CHANNELS = '/etc/igmpproxy-channels.m3u';
var MCAST = /\b(2(?:2[4-9]|3\d)\.\d{1,3}\.\d{1,3}\.\d{1,3})\b/;

var callLeases = rpc.declare({
	object: 'luci-rpc',
	method: 'getDHCPLeases',
	expect: { '': {} }
});

/*
 * Channel list: M3U (#EXTINF title followed by a rtp://, udp:// or udpxy
 * http://.../rtp/ URL), or plain lines "239.1.1.1 Name", "239.1.1.1,Name",
 * "Name;239.1.1.1:5000". Returns { ip: name }.
 */
function parseChannels(text) {
	var map = {}, pending = null;
	(text || '').split(/\r?\n/).forEach(function(line) {
		line = line.trim();
		if (!line)
			return;
		if (/^#EXTINF/i.test(line)) {
			var comma = line.lastIndexOf(','), tvg = line.match(/tvg-name="([^"]+)"/i);
			pending = (comma >= 0 ? line.slice(comma + 1).trim() : '') || (tvg ? tvg[1] : null);
			return;
		}
		if (line.charAt(0) == '#')
			return;
		var ip = line.match(MCAST);
		if (!ip) {
			pending = null;
			return;
		}
		var name = pending;
		if (!name)
			name = line.replace(/[^\s,;|]*\b2(?:2[4-9]|3\d)\.\d{1,3}\.\d{1,3}\.\d{1,3}[^\s,;|]*/, '')
				.replace(/^[\s,;:|\t-]+|[\s,;:|\t-]+$/g, '').trim();
		if (name)
			map[ip[1]] = name;
		pending = null;
	});
	return map;
}

function pad(n) { return (n < 10 ? '0' : '') + n; }

return baseclass.extend({
	CHANNELS: CHANNELS,
	parseChannels: parseChannels,

	loadChannelText: function() {
		return L.resolveDefault(fs.read(CHANNELS), '');
	},

	loadChannels: function() {
		return this.loadChannelText().then(parseChannels);
	},

	saveChannels: function(text) {
		return fs.write(CHANNELS, text || '');
	},

	fetchNames: function() {
		return L.resolveDefault(callLeases(), {}).then(function(res) {
			var names = {};
			(res.dhcp_leases || []).forEach(function(l) {
				if (l.ipaddr && l.hostname)
					names[l.ipaddr] = l.hostname;
			});
			return names;
		});
	},

	/* the whole event log; exec_direct streams over HTTP (no ubus size limit) */
	fetchEvents: function() {
		return fs.exec_direct('/usr/sbin/igmpproxy', ['-E'], 'text').then(function(text) {
			var events = [];
			(text || '').split('\n').forEach(function(line) {
				if (line)
					try { events.push(JSON.parse(line)); } catch (e) {}
			});
			if (events.length == 1 && events[0].error)
				return { error: events[0].error, events: [], raw: text };
			return { events: events, raw: text };
		}).catch(function(e) {
			return { error: e.message || _('IGMP proxy is not running'), events: [], raw: '' };
		});
	},

	localTime: function(ts) {
		var d = new Date(ts);
		if (isNaN(d))
			return ts;
		return '%d-%s-%s %s:%s:%s'.format(d.getFullYear(), pad(d.getMonth() + 1), pad(d.getDate()),
			pad(d.getHours()), pad(d.getMinutes()), pad(d.getSeconds()));
	},

	/* "RTP 1" with the address as tooltip, or the address */
	channel: function(ip, channels) {
		var n = channels && channels[ip];
		return n ? E('span', { 'title': ip }, [ E('strong', {}, n), ' ', E('small', { 'style': 'opacity:.6' }, ip) ])
		         : E('code', {}, ip);
	},

	channelText: function(ip, channels) {
		return (channels && channels[ip]) ? '%s (%s)'.format(channels[ip], ip) : ip;
	},

	mbps: function(bps) {
		if (bps == null)
			return '…';
		if (bps < 1e6)
			return '%.0f kbit/s'.format(bps / 1e3);
		return '%.1f Mbit/s'.format(bps / 1e6);
	},

	/*
	 * Possible problems in the last 'hours' of the event log. Each issue:
	 * { level: 'warn'|'info', ts, text }. Rules:
	 *  - a stream stopped and was asked for again on the same interface
	 *    within 60 s: viewers most likely saw a black screen
	 *  - a device timed out but joined the same stream again within 120 s:
	 *    it was still watching but its reports did not arrive
	 *  - the proxy started more than once
	 */
	analyze: function(events, names, channels, hours) {
		var self = this, issues = [], since = Date.now() - (hours || 24) * 3600e3;
		var ev = events.filter(function(e) { return Date.parse(e.ts) >= since; });
		var dev = function(ip) { return names[ip] || ip; };
		var REASON = {
			timeout: _('no device reported in time'),
			no_answer: _('no device answered after a leave'),
			fast_leave: _('fast leave')
		};

		ev.forEach(function(e, i) {
			var t = Date.parse(e.ts);
			if (e.ev == 'forward_stop') {
				for (var j = i + 1; j < ev.length; j++) {
					var f = ev[j], dt = (Date.parse(f.ts) - t) / 1000;
					if (dt > 60)
						break;
					if ((f.ev == 'forward_start' || f.ev == 'join') && f.group == e.group && f['if'] == e['if']) {
						issues.push({ level: 'warn', ts: e.ts, text:
							_('%s stopped (%s) and %s asked for it again %d s later: a viewer probably saw a black screen').format(
								self.channelText(e.group, channels), REASON[e.reason] || e.reason, dev(f.host), Math.round(dt)) });
						break;
					}
				}
			}
			else if (e.ev == 'host_timeout') {
				for (var k = i + 1; k < ev.length; k++) {
					var g = ev[k], dk = (Date.parse(g.ts) - t) / 1000;
					if (dk > 120)
						break;
					if (g.ev == 'join' && g.host == e.host && g.group == e.group) {
						issues.push({ level: 'warn', ts: e.ts, text:
							_('%s timed out on %s but joined again %d s later: its reports are getting lost (Wi-Fi, switch or a busy device)').format(
								dev(e.host), self.channelText(e.group, channels), Math.round(dk)) });
						break;
					}
				}
			}
		});

		var refused = {};
		ev.forEach(function(e) {
			if (e.ev == 'upstream_join_failed')
				refused[e.group] = e;
		});
		Object.keys(refused).forEach(function(g) {
			issues.push({ level: 'warn', ts: refused[g].ts, text:
				_('%s could not be requested from the provider (%s)').format(self.channelText(g, channels), refused[g].error) });
		});

		var starts = ev.filter(function(e) { return e.ev == 'start'; });
		if (starts.length > 1)
			issues.push({ level: 'info', ts: starts[starts.length - 1].ts, text:
				_('The proxy started %d times (settings saved, interface changes or restarts); every restart briefly interrupts streams').format(starts.length) });

		return issues.sort(function(a, b) { return a.ts < b.ts ? 1 : -1; });
	},

	issueBox: function(issues, hours, max) {
		var self = this;
		if (!issues.length)
			return E('div', { 'class': 'alert-message notice', 'style': 'margin-bottom:1em' },
				_('No problems found in the last %d hours.').format(hours));
		var list = issues.slice(0, max || 20);
		return E('div', { 'class': 'alert-message warning', 'style': 'margin-bottom:1em' }, [
			E('strong', {}, _('%d possible problem(s) in the last %d hours').format(issues.length, hours)),
			E('ul', { 'style': 'margin:.5em 0 0 1.2em' }, list.map(function(i) {
				return E('li', {}, [ E('span', { 'style': 'opacity:.7' }, self.localTime(i.ts) + ' '), i.text ]);
			})),
			issues.length > list.length ? E('em', {}, _('and %d more').format(issues.length - list.length)) : ''
		]);
	}
});

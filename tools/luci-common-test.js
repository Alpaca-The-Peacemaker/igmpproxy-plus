// Unit test for the LuCI helper module (channel names, issue detection).
// usage: node tools/luci-common-test.js openwrt/luci/.../resources/igmpproxy/common.js
const fs = require('fs');
const src = fs.readFileSync(process.argv[2], 'utf8');

String.prototype.format = function() {
  let i = 0; const a = arguments;
  return this.replace(/%(\.\d+)?([sdf])/g, (m, p, t) => {
    const v = a[i++];
    if (t === 'f') return Number(v).toFixed(p ? +p.slice(1) : 6);
    if (t === 'd') return String(Math.trunc(v));
    return String(v);
  });
};
global._ = s => s;
global.E = (tag, attrs, kids) => ({ tag, kids });
global.L = { resolveDefault: p => p, toArray: v => v == null ? [] : Array.isArray(v) ? v : String(v).split(/\s+/).filter(x => x) };
const rpc = { declare: () => () => Promise.resolve({}) };
const baseclass = { extend: o => o };
const mod = new Function('baseclass', 'fs', 'rpc', src)(baseclass, {}, rpc);

const t0 = Date.now() - 3600e3;
const ts = s => new Date(t0 + s * 1000).toISOString();
const events = [
  { ts: ts(0), ev: 'start' },
  { ts: ts(10), ev: 'join', group: '239.1.1.1', if: 'br-lan', host: '192.168.1.84' },
  { ts: ts(300), ev: 'forward_stop', group: '239.1.1.1', if: 'br-lan', reason: 'timeout' },
  { ts: ts(302), ev: 'forward_start', group: '239.1.1.1', if: 'br-lan', host: '192.168.1.84' },
  { ts: ts(400), ev: 'host_timeout', group: '239.1.1.2', if: 'br-lan', host: '192.168.1.83' },
  { ts: ts(430), ev: 'join', group: '239.1.1.2', if: 'br-lan', host: '192.168.1.83' },
  { ts: ts(500), ev: 'forward_stop', group: '239.1.1.3', if: 'br-lan', reason: 'fast_leave' },
  { ts: ts(900), ev: 'join', group: '239.1.1.3', if: 'br-lan', host: '192.168.1.83' }, // > 60 s: no issue
  { ts: ts(1000), ev: 'start' },
  { ts: new Date(Date.now() - 30 * 3600e3).toISOString(), ev: 'start' } // older than 24 h
];
events.sort((a, b) => a.ts < b.ts ? -1 : 1);
const issues = mod.analyze(events, { '192.168.1.84': 'tv-sala' }, { '239.1.1.1': 'RTP 1' }, 24);
issues.forEach(i => console.log(i.level, '|', i.text));
let ok = issues.length === 3 &&
  issues.some(i => /RTP 1 \(239\.1\.1\.1\) stopped \(no device reported in time\) and tv-sala asked for it again 2 s later/.test(i.text)) &&
  issues.some(i => /192\.168\.1\.83 timed out on 239\.1\.1\.2 but joined again 30 s later/.test(i.text)) &&
  issues.some(i => /started 2 times/.test(i.text));
console.log(mod.mbps(null), mod.mbps(512e3), mod.mbps(8.26e6));
ok = ok && mod.mbps(8.26e6) === '8.3 Mbit/s' && mod.mbps(512e3) === '512 kbit/s';
const ch = mod.parseChannels('#EXTM3U\n#EXTINF:-1 tvg-name="x",RTP 1\nrtp://@239.1.1.1:5000\n' +
  '#EXTINF:-1,SIC HD\nhttp://192.168.1.1:4022/rtp/239.1.1.2:5000\n239.1.1.3 TVI\n' +
  '239.1.1.4:5000,CNN Portugal\nEuronews;udp://@239.1.1.5:1234\n# note\nhttp://example.com/x\n');
const want = { '239.1.1.1': 'RTP 1', '239.1.1.2': 'SIC HD', '239.1.1.3': 'TVI', '239.1.1.4': 'CNN Portugal', '239.1.1.5': 'Euronews' };
const chOk = JSON.stringify(ch) === JSON.stringify(want);
if (!chOk) console.log('channel parser got', ch);
ok = ok && chOk;
const wl = mod.whitelistFromChannels({ '239.195.5.36': 'a', '239.195.5.47': 'b', '239.196.6.19': 'c' });
ok = ok && JSON.stringify(wl) === JSON.stringify(['239.195.5.0/24', '239.196.6.0/24']);
const slow = mod.analyze([{ ts: ts(50), ev: 'first_packet', group: '239.1.1.1', delay_ms: 4200 },
  { ts: ts(60), ev: 'first_packet', group: '239.1.1.2', delay_ms: 300 }], {}, { '239.1.1.1': 'RTP 1' }, 24);
ok = ok && slow.length === 1 && /RTP 1 \(239\.1\.1\.1\) took 4\.2 s to start/.test(slow[0].text);
// 1.6.0: per network policy, networks, groups per network, stalls, network names
const cn = ['239.195.5.0/24', '239.196.6.0/24'];
const pol = [
  mod.policyOf('querier', null, null, cn) === 'none',
  mod.policyOf('downstream', null, null, cn) === 'all',
  mod.policyOf('downstream', ['239.196.6.0/24', '239.195.5.0/24'], null, cn) === 'channels',
  mod.policyOf('downstream', '239.195.5.0/24 239.196.6.0/24', null, cn) === 'channels',
  mod.policyOf('downstream', cn, ['239.255.255.250/32'], cn) === 'custom',
  mod.policyOf('downstream', ['239.1.1.0/24'], null, cn) === 'custom',
  mod.policyOf('downstream', ['239.1.1.0/24'], null, []) === 'custom',
];
if (!pol.every(x => x)) console.log('policyOf', pol);
ok = ok && pol.every(x => x);
const st = { interfaces: [
  { name: 'eth1', state: 'upstream' }, { name: 'br-lan', state: 'downstream' }, { name: 'br-iot', state: 'querier' }
], groups: [
  { group: '239.1.1.1', members: [{ interface: 'br-lan' }] },
  { group: '239.1.1.2', members: [{ interface: 'br-tv' }] }
] };
const nets = mod.networksOf(st).map(i => i.name).join(',');
const on = mod.groupsOn(st.groups, 'br-lan').map(g => g.group).join(',');
ok = ok && nets === 'br-lan,br-iot' && on === '239.1.1.1' && mod.networksOf({}).length === 0;
if (!(nets === 'br-lan,br-iot' && on === '239.1.1.1')) console.log('networks', nets, on);
const ev3 = [
  { ts: ts(10), ev: 'join', group: '239.1.1.1', if: 'br-lan', host: '192.168.1.84' },
  { ts: ts(11), ev: 'join', group: '239.1.1.5', if: 'br-tv', host: '192.168.5.10' },
  { ts: ts(20), ev: 'stream_stalled', group: '239.1.1.1', if: 'eth1', silent_s: 12 },
  { ts: ts(300), ev: 'forward_stop', group: '239.1.1.5', if: 'br-tv', reason: 'timeout' },
  { ts: ts(305), ev: 'join', group: '239.1.1.5', if: 'br-tv', host: '192.168.5.10' },
];
const is3 = mod.analyze(ev3, {}, { '239.1.1.1': 'RTP 1' }, 24);
is3.forEach(i => console.log(i.level, '|', i.text));
const stallOk = is3.some(i => /RTP 1 \(239\.1\.1\.1\): no data from the provider for 12 s/.test(i.text));
const netOk = is3.some(i => /black screen \(network br-tv\)$/.test(i.text));
if (!stallOk || !netOk) console.log('stall/network messages', stallOk, netOk);
ok = ok && stallOk && netOk && is3.length === 2;
console.log(ok ? 'LuCI helper tests passed' : 'LuCI helper tests FAILED');
process.exit(ok ? 0 : 1);

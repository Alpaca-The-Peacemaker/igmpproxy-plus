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
global.L = { resolveDefault: p => p };
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
console.log(ok ? 'LuCI helper tests passed' : 'LuCI helper tests FAILED');
process.exit(ok ? 0 : 1);

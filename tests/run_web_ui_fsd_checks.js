// Run with node --test tests/run_web_ui_fsd_checks.js.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const html = fs.readFileSync(path.join(__dirname, '../include/web_ui_fsd.h'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const flush = () => new Promise(resolve => setImmediate(resolve));

async function loadFsdPage(routes) {
  const nodes = new Map();
  const body = { innerHTML: '', rows: [] };
  function get(id) {
    if (!nodes.has(id)) {
      nodes.set(id, {
        value: '', textContent: '', innerHTML: '', style: {}, checked: false,
        className: '',
        querySelectorAll: () => [],
        appendChild() {},
        addEventListener(event, handler) { this['on' + event] = handler; },
      });
    }
    return nodes.get(id);
  }
  function response(bodyText, status = 200) {
    return { ok: status === 200, status, text: async () => bodyText };
  }
  vm.runInNewContext(script, {
    document: {
      hidden: false,
      getElementById: get,
      querySelector: () => body,
      querySelectorAll: () => [],
      addEventListener() {},
    },
    sessionStorage: { getItem: () => '', setItem() {} },
    confirm: () => true,
    setTimeout() {},
    setInterval() { return 1; },
    clearInterval() {},
    async fetch(url) {
      const { pathname } = new URL(url, 'http://device');
      if (pathname === '/api/time') return response('OK');
      if (!(pathname in routes)) assert.fail(`Unexpected fetch ${pathname}`);
      return response(routes[pathname]);
    },
  });
  await flush();
  return { get };
}

test('timeline displays persisted config changes and car pulls after reboot when RAM hits are empty', async () => {
  const tlLines = [
    JSON.stringify({
      ts: 'up:18',
      kind: 'HIT',
      method: 'GET',
      vin: 'LRW3E7FS0NC000001',
      req_head: '{"ver":1}',
      req_bytes: 9,
      bytes: 654,
      mode: 'reply',
      md5: 'bd262907ff9fb2709bb54f9d3e78d07d',
      http_version: 'HTTP/1.1',
      status: 200,
    }),
    JSON.stringify({
      ts: '2026-11-28T03:45:00+0800',
      kind: 'CFG',
      source: 'config',
      bytes: 654,
      md5: 'bd262907ff9fb2709bb54f9d3e78d07d',
      fields: 22,
    }),
    JSON.stringify({
      ts: '2026-11-28T03:40:00+0800',
      kind: 'OBSERVE',
      method: 'POST',
      vin: 'LRW3E7FS0NC000001',
      req_head: 'hello<script>',
      req_bytes: 12,
      bytes: 502,
      mode: 'observe',
      md5: '11223344556677889900aabbccddeeff',
      http_version: 'HTTP/1.1',
      status: 502,
    }),
  ].join('\n') + '\n';

  const { get } = await loadFsdPage({
    '/api/wifi-bridge/status': JSON.stringify({
      upstreamConnected: false,
      fsdMock: { enabled: false, active: false, hits: 0, observes: 0, flows: 0, bytes: 654, fields: 22, md5: 'bd262907ff9fb2709bb54f9d3e78d07d', recent: [] },
    }),
    '/api/fsd-mock/hits': '',
    '/api/fsd-mock/handshakes': '',
    '/api/fsd-mock/config-history': JSON.stringify({
      ts: '2026-11-28T03:45:00+0800', source: 'config', bytes: 654, md5: 'bd262907ff9fb2709bb54f9d3e78d07d', fields: 22,
    }) + '\n',
    '/api/fsd-mock/timeline': tlLines,
    '/api/fsd-mock/payload': '{"full_self_driving":true}',
  });

  const tlHtml = get('tlRows').innerHTML;
  const rows = tlHtml.match(/<tr>.*?<\/tr>/g) || [];
  assert.equal(rows.length, 3);
  // Preserves firmware chronological order (up:18 stays newest ahead of ISO timestamp).
  assert.match(rows[0], /up:18.*车机取配置.*GET.*LRW3E7FS0NC000001.*654 B \/ reply/);
  assert.match(rows[1], /2026-11-28 03:45:00.*配置变更.*config.*22 字段.*654 B.*本地保存/);
  assert.match(rows[2], /2026-11-28 03:40:00.*车机取配置.*POST.*502 · 502 B \/ observe/);
  assert.ok(!tlHtml.includes('<script>'), 'timeline must HTML-escape req_head');

  // Cards fall back to persisted timeline hit metadata after reboot while cCar stays offline.
  assert.equal(get('cLastHit').textContent, 'up:18');
  assert.equal(get('cVin').textContent, 'LRW3E7FS0NC000001');
  assert.equal(get('cCar').textContent, '离线 · 等待车机请求');
});

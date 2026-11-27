// Run with node --test tests/run_web_ui_manage_checks.js.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const html = fs.readFileSync(path.join(__dirname, '../include/web_ui_manage.h'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const flush = () => new Promise(resolve => setImmediate(resolve));

async function loadPage() {
  const nodes = new Map();
  const intervals = new Map();
  const writes = [];
  const state = { dnsEnable: true, dnsAllow: 'example.com', dnsBlock: 'blocked.example' };
  let now = 0;
  let nextTimerId = 0;
  let rejectDisable = false;
  function get(id) {
    if (!nodes.has(id)) {
      nodes.set(id, {
        value: '', textContent: '', style: {}, checked: false, open: false,
        matches: () => false,
        showModal() { this.open = true; },
        close() { this.open = false; },
        addEventListener(event, handler) { this['on' + event] = handler; },
      });
    }
    return nodes.get(id);
  }
  function response(body, status = 200) {
    return { ok: status === 200, status, text: async () => body };
  }
  vm.runInNewContext(script, {
    document: { getElementById: get, querySelectorAll: () => [], addEventListener() {} },
    sessionStorage: { getItem: () => '', setItem() {} },
    performance: { now: () => now },
    setTimeout() {},
    setInterval(fn, delay) { intervals.set(++nextTimerId, { fn, delay }); return nextTimerId; },
    clearInterval(id) { intervals.delete(id); },
    async fetch(url) {
      const { pathname, searchParams } = new URL(url, 'http://device');
      switch (pathname) {
        case '/api/wifi-bridge/status': {
          const reasons = [];
          const allowCount = state.dnsAllow.split(/[\s,;]+/).filter(Boolean).length;
          if (!state.dnsEnable) reasons.push('dns_filter_disabled');
          if (allowCount === 0) reasons.push('dns_allowlist_empty');
          return response(JSON.stringify({ ...state, dnsAllowCount: allowCount, apInternetBlockReasons: reasons }));
        }
        case '/api/wifi-bridge/set': {
          searchParams.delete('token');
          const change = Object.fromEntries(searchParams);
          writes.push(change);
          if (change.dnsEnable === '0' && rejectDisable) return response('UNAUTH', 403);
          if ('dnsEnable' in change) state.dnsEnable = change.dnsEnable === '1';
          if ('dnsAllow' in change) state.dnsAllow = change.dnsAllow;
          if ('dnsBlock' in change) state.dnsBlock = change.dnsBlock;
          if (change.dnsReset === 'allow') {
            state.dnsAllow = 'default.example';
            state.dnsEnable = true;
          }
          return response('OK');
        }
        case '/api/auth':
          rejectDisable = false;
          return response('test-token');
        default:
          assert.fail(`Unexpected URL ${url}`);
      }
    },
  });
  await flush();
  return {
    get, writes, state,
    rejectDisable() { rejectDisable = true; },
    tick(ms) {
      now += ms;
      for (const { fn, delay } of intervals.values()) if (delay === 250) fn();
    },
    toggleDns(enabled) {
      get('dnsEn').checked = enabled;
      get('dnsEn').onchange();
    },
  };
}

test('disabling requires ten seconds, acknowledgment, and one successful submit', async () => {
  const page = await loadPage();
  const { get, writes } = page;
  const dialog = get('dnsDisableDialog');
  const ack = get('dnsDisableAck');
  const submit = get('dnsDisableSubmit');
  assert.match(html, /\[!!危险!!\] 确认禁用/);

  page.toggleDns(false);
  assert.ok(dialog.open && get('dnsEn').checked);
  assert.ok(ack.disabled && submit.disabled);
  page.tick(9999);
  assert.ok(ack.disabled && submit.disabled);
  submit.onclick();
  page.tick(1);
  assert.ok(!ack.disabled && submit.disabled);
  submit.onclick();
  assert.deepEqual(writes, []);

  ack.checked = true;
  ack.onchange();
  assert.ok(!submit.disabled);
  submit.onclick();
  submit.onclick();
  await flush();
  assert.deepEqual(writes, [{ dnsEnable: '0' }]);
  assert.ok(!dialog.open && !get('dnsEn').checked);
  assert.equal(get('apInternetWarning').hidden, false);
  assert.match(get('apInternetWarningDetails').textContent, /DNS 过滤已关闭/);
});

test('cancel, Escape, and PIN rejection require a fresh confirmation', async () => {
  const page = await loadPage();
  const { get, writes, state } = page;
  const dialog = get('dnsDisableDialog');
  const ack = get('dnsDisableAck');
  for (const cancel of [get('dnsDisableCancel').onclick, () => dialog.oncancel({ preventDefault() {} })]) {
    page.toggleDns(false);
    page.tick(10000);
    ack.checked = true;
    cancel();
    assert.ok(!dialog.open && !ack.checked && state.dnsEnable);
  }
  assert.deepEqual(writes, []);

  page.rejectDisable();
  page.toggleDns(false);
  assert.match(get('dnsDisableCountdown').textContent, /10 秒/);
  page.tick(10000);
  ack.checked = true;
  get('dnsDisableSubmit').onclick();
  await flush();
  assert.ok(state.dnsEnable && get('dnsEn').checked && !dialog.open);
  assert.equal(get('pinOverlay').style.display, 'flex');
  await get('pinBtn').onclick();
  await flush();
  assert.deepEqual(writes, [{ dnsEnable: '0' }]);
  page.toggleDns(false);
  assert.ok(dialog.open && ack.disabled && !ack.checked);
});

test('upstream and list saves are independent and reason banners use saved values', async () => {
  const page = await loadPage();
  const { get, writes, state } = page;
  get('dnsAllow').value = ' ; , ';
  get('upEn').checked = true;
  get('upEn').onchange();
  await flush();
  assert.equal(get('dnsAllow').value, ' ; , ');
  assert.equal(get('apInternetWarning').hidden, true);

  await get('dnsSave').onclick();
  await flush();
  assert.deepEqual(writes, [{ upstreamEnable: '1' }, { dnsAllow: ' ; , ', dnsBlock: 'blocked.example' }]);
  assert.equal(get('apInternetWarning').hidden, false);
  assert.match(get('apInternetWarningDetails').textContent, /白名单为空/);
  assert.ok(state.dnsEnable);

  page.toggleDns(false);
  page.tick(10000);
  get('dnsDisableAck').checked = true;
  get('dnsDisableSubmit').onclick();
  await flush();
  assert.match(get('apInternetWarningDetails').textContent, /DNS 过滤已关闭，白名单为空/);
  get('dnsResetAllow').onclick();
  await flush();
  assert.deepEqual(writes.at(-1), { dnsReset: 'allow' });
  assert.ok(get('dnsEn').checked && get('apInternetWarning').hidden);
  assert.equal(get('dnsAllow').value, 'default.example');
});

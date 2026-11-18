import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
import test from 'node:test';

const html = readFileSync(new URL('../include/web_ui_service.h', import.meta.url), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const unavailable = { protocolVerified: false, entrySupported: false, exitSupported: false, safeMode: false };
const response = (status, data) => ({ status, ok: status >= 200 && status < 300,
  json: async () => data, text: async () => data });
const flush = async () => { for (let i = 0; i < 4; i++) await new Promise(setImmediate); };

async function page(handler = () => response(200, unavailable), options = {}) {
  const elements = new Map();
  const document = { activeElement: null, addEventListener() {}, getElementById(id) { return elements.get(id); } };
  for (const match of html.matchAll(/<([\w]+)\b([^>]*\bid="([^"]+)"[^>]*)>/g)) {
    const [, , attributes, id] = match;
    elements.set(id, { id, disabled: /\bdisabled\b/.test(attributes), hidden: /\bhidden\b/.test(attributes),
      checked: false, textContent: '', value: '', href: id === 'back' ? '/' : '',
      focus() { document.activeElement = this; } });
  }
  const calls = [], storage = new Map();
  const sessionStorage = { getItem(key) { if (options.storageBlocked) throw Error('blocked'); return storage.get(key); },
    setItem(key, value) { if (options.storageBlocked) throw Error('blocked'); storage.set(key, value); } };
  const context = vm.createContext({ document, sessionStorage, URLSearchParams, AbortController, setTimeout, clearTimeout,
    setInterval() {},
    location: { search: options.search || '' }, fetch: async (url, init = {}) => {
      calls.push({ url, ...init }); return handler(url, init);
    } });
  vm.runInContext(script, context);
  await flush();
  return { context, calls, elements, storage, get: id => elements.get(id) };
}

test('unverified protocol never enables actions, including direct JS calls', async () => {
  const p = await page();
  assert.equal(p.get('enter').disabled, true);
  assert.equal(p.get('exit').disabled, true);
  assert.match(p.get('connection').textContent, /协议未验证/);
  p.context.openConfirm('enter');
  assert.equal(p.get('confirmOverlay').hidden, true);
  p.context.pendingAction = 'enter'; p.get('parked').checked = true;
  await p.context.execute();
  assert.equal(p.calls.filter(call => call.method === 'POST').length, 0);
});

test('unexpected, malformed or failed capability responses leave actions disabled', async () => {
  for (const handler of [
    () => response(200, {}),
    () => response(200, { protocolVerified: true, entrySupported: true, exitSupported: true }),
    () => response(500, unavailable),
    () => { throw Error('offline'); },
    () => ({ ok: true, status: 200, json: async () => { throw Error('bad json'); } }),
  ]) {
    const p = await page(handler);
    assert.equal(p.get('enter').disabled, true);
    assert.equal(p.get('exit').disabled, true);
    assert.equal(p.context.capability, null);
    assert.match(p.get('connection').textContent, /无法确认/);
    assert.equal(p.get('refresh').disabled, false);
  }
});

test('safe mode, car return link and blocked browser storage are handled', async () => {
  const p = await page(() => response(200, { ...unavailable, safeMode: true }),
    { storageBlocked: true, search: '?from=car' });
  assert.equal(p.get('back').href, '/car');
  assert.match(p.get('device').textContent, /安全模式/);
  assert.equal(p.get('enter').disabled, true);
});

test('PIN failure does not close the dialog; valid auth refreshes capability', async () => {
  let authenticated = false;
  const p = await page((url, init) => {
    if (url === '/api/auth') {
      if (init.body.get('pin') !== '1234') return response(403, 'WRONG');
      authenticated = true; return response(200, '12345678ABCDEF00');
    }
    return response(authenticated ? 200 : 403, unavailable);
  });
  assert.equal(p.get('pinOverlay').hidden, false);
  p.get('pin').value = 'bad'; await p.get('pinForm').onsubmit({ preventDefault() {} });
  assert.match(p.get('pinError').textContent, /PIN 错误/);
  assert.equal(p.get('pinOverlay').hidden, false);
  p.get('pin').value = '1234'; await p.get('pinForm').onsubmit({ preventDefault() {} }); await flush();
  assert.equal(p.get('pinOverlay').hidden, true);
  assert.equal(p.storage.get('fsd_tok'), '12345678ABCDEF00');
  assert.match(p.calls.at(-1).url, /token=12345678ABCDEF00/);
  assert.equal(p.get('enter').disabled, true);
});

test('confirmation cancel, missing checkbox and double submit cannot duplicate commands', async () => {
  let finish;
  const p = await page((url, init) => init.method === 'POST'
    ? new Promise(resolve => { finish = resolve; }) : response(200, unavailable));
  // Exercise the reserved dialog in isolation. The real firmware never grants
  // this capability; the tests above ensure a GET cannot enable it today.
  p.context.capability = { protocolVerified: true, entrySupported: true, exitSupported: true };
  p.context.openConfirm('enter'); p.context.closeConfirm();
  assert.equal(p.calls.filter(call => call.method === 'POST').length, 0);
  p.context.openConfirm('exit'); await p.context.execute();
  assert.equal(p.calls.filter(call => call.method === 'POST').length, 0);
  p.get('parked').checked = true;
  const first = p.context.execute(); await p.context.execute();
  const writes = p.calls.filter(call => call.method === 'POST');
  assert.equal(writes.length, 1);
  assert.equal(writes[0].body.get('action'), 'exit');
  assert.equal(writes[0].body.get('confirm'), 'EXIT_SERVICE_MODE');
  finish(response(501, { error: 'protocol_unverified', transmitted: false })); await first;
  assert.match(p.get('result').textContent, /未发送控制指令/);
  assert.equal(p.get('enter').disabled, true);
  assert.equal(p.context.capability, null);
});

test('network error after confirmation never claims success or retries', async () => {
  const p = await page((url, init) => {
    if (init.method === 'POST') throw Error('offline');
    return response(200, unavailable);
  });
  p.context.capability = { protocolVerified: true, entrySupported: true, exitSupported: true };
  p.context.openConfirm('enter'); p.get('parked').checked = true;
  await p.context.execute(); await flush();
  assert.match(p.get('result').textContent, /结果未知/);
  assert.equal(p.calls.filter(call => call.method === 'POST').length, 1);
});

const observed = { ...unavailable, telemetryVerified: false, signalSource: 'legacy_sx_di_torque2',
  signalStatus: 'observed', signalFresh: true, signalAgeMs: 10,
  frameDlc: 6, frameHex: '00 10 F4 01 00 1E', gear: 'P', speedKph: 0, parked: true };

test('live S/X gear and signed speed are displayed without enabling actions', async () => {
  let data = observed;
  const p = await page(() => response(200, data));
  assert.equal(p.get('gear').textContent, 'P');
  assert.equal(p.get('speed').textContent, '0.00 km/h');
  assert.equal(p.get('parkedState').textContent, '观察到 P 挡且静止');
  assert.match(p.get('signalDetails').textContent, /00 10 F4 01 00 1E/);
  assert.equal(p.get('enter').disabled, true);
  data = { ...observed, gear: 'R', speedKph: -0.805, parked: false };
  await p.context.refresh();
  assert.equal(p.get('gear').textContent, 'R');
  assert.equal(p.get('speed').textContent, '-0.81 km/h');
  assert.match(p.get('parkedState').textContent, /未同时满足/);
});

test('stale, unavailable, unsupported and malformed observations never retain Park', async () => {
  for (const signalStatus of ['no_signal', 'stale_signal', 'can_unavailable',
    'can_session_changed', 'checksum_mismatch', 'unsupported_frame_layout', 'unexpected']) {
    let data = observed;
    const p = await page(() => response(200, data));
    data = { ...observed, signalStatus };
    await p.context.refresh();
    assert.equal(p.get('gear').textContent, '未知');
    assert.equal(p.get('speed').textContent, '未知');
    assert.equal(p.get('parkedState').textContent, '未知');
    assert.equal(p.get('enter').disabled, true);
  }
  for (const patch of [{ speedKph: null }, { speedKph: '0' }, { speedKph: NaN },
    { gear: 'SNA' }, { gear: null }, { signalFresh: false }, { signalSource: 'model3' }]) {
    const p = await page(() => response(200, { ...observed, ...patch }));
    assert.equal(p.get('parkedState').textContent, '未知');
  }
});

test('refresh immediately clears old Park while a request is pending or fails', async () => {
  let handler = () => response(200, observed);
  const p = await page((...args) => handler(...args));
  let finish;
  handler = () => new Promise(resolve => { finish = resolve; });
  const pending = p.context.refresh();
  assert.equal(p.get('gear').textContent, '未知');
  assert.equal(p.get('parkedState').textContent, '未知');
  finish(response(500, {})); await pending;
  assert.equal(p.get('parkedState').textContent, '未知');
  assert.equal(p.get('vehicleSignal').textContent, '连接失败');
});

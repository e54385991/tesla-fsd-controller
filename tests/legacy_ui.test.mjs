import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
import test from 'node:test';

function fragment(text, start, end) {
  assert.ok(text.includes(start) && text.includes(end));
  return start + text.split(start)[1].split(end)[0];
}

function page(kind, d) {
  const html = readFileSync(new URL(`../include/web_ui${kind === 'car' ? '_car' : ''}.h`, import.meta.url), 'utf8');
  const elements = new Map();
  for (const match of html.matchAll(/\bid="([^"]+)"/g)) {
    const classes = new Set();
    elements.set(match[1], { style: {}, textContent: '', checked: false, value: '',
      classList: { toggle(name, on) { if (on) classes.add(name); else classes.delete(name); },
        contains(name) { return classes.has(name); } } });
  }
  const document = { activeElement: null, getElementById(id) { return elements.get(id); } };
  const context = vm.createContext({ document, d, legacyDirty: false, legB7Dirty: false,
    LEGACY_DEF: 0, LEGB7_DEF: 15 });
  if (kind === 'car') {
    const toggle = fragment(html, 'function setTog(id, on){', '\n}');
    vm.runInContext(toggle + '\n}', context);
    vm.runInContext(fragment(html, "  setTog('tgRvsl'", '  // HW 选中'), context);
  } else {
    vm.runInContext(fragment(html, '    var rvslEl=', '    if(d.tempSeen)'), context);
  }
  return { get: id => elements.get(id), context, html };
}

for (const kind of ['phone', 'car']) {
  test(`${kind}: byte7 false/zero and configured values are rendered from status`, () => {
    const d = { hwMode: 0, legacyByte7Enable: 0, legacyByte7Value: 0, legacyOffset: 33,
      removeVSL: 0, gps2F8Seen: true, gps2F8UserOffRaw: 40, gps2F8MppLimRaw: 12,
      gps2F8UserUnits: 1, gps2F8MapUnits: 0 };
    const p = page(kind, d);
    if (kind === 'phone') {
      assert.equal(p.get('legacyByte7Enable').checked, false);
      assert.equal(p.get('legacyByte7Value').value, '0');
      assert.equal(p.get('legacyOffset').value, 33);
    } else {
      assert.equal(p.get('tgLegB7En').classList.contains('on'), false);
      assert.equal(Number(p.get('cLegB7').textContent), 0);
      assert.equal(Number(p.get('cLegOff').textContent), 33);
    }
    d.legacyByte7Enable = 1; d.legacyByte7Value = 3;
    const changed = page(kind, d);
    if (kind === 'phone') {
      assert.equal(changed.get('legacyByte7Enable').checked, true);
      assert.equal(changed.get('legacyByte7Value').value, '3');
    } else {
      assert.equal(changed.get('tgLegB7En').classList.contains('on'), true);
      assert.equal(Number(changed.get('cLegB7').textContent), 3);
    }
  });

  test(`${kind}: OEM offset and map-limit units are independent`, () => {
    for (const userUnits of [0, 1]) for (const mapUnits of [0, 1]) {
      const p = page(kind, { hwMode: 0, gps2F8UserOffRaw: 40, gps2F8MppLimRaw: 12,
        gps2F8UserUnits: userUnits, gps2F8MapUnits: mapUnits });
      const prefix = kind === 'car' ? 'c' : 's';
      assert.equal(p.get(`${prefix}Gps2F8UserOff`).textContent, `40 (=10 ${userUnits ? 'mph' : 'kph'})`);
      assert.equal(p.get(`${prefix}Gps2F8MppLim`).textContent, `12 (=60 ${mapUnits ? 'mph' : 'kph'})`);
    }
  });
}

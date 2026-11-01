#!/usr/bin/env node
// Unit tests of the pure functions in web/flash/config.js (the console line parser, the JSON extraction of the
// `cfg` answer, the colour helpers). No dependencies:   node web/flash/config.test.js
'use strict';
const assert = require('assert');
const path = require('path');
const c = require(path.join(__dirname, 'config.js'));

let n = 0;
function t(name, fn) { try { fn(); n++; } catch (e) { console.error('FAIL ' + name + ': ' + e.message); process.exitCode = 1; } }

const CFG = '{"color":"#ff8000","color2":"auto","dev":false,"name":"","bright":80,"version":"v0.1.0-rc.0","addr":"aa:bb:cc:dd:ee:ff","peers":[]}';

t('ESP-IDF prefix', () => assert.deepStrictEqual(c.parseLine('I (1234) test: cfg ' + CFG), { tag: 'test', text: 'cfg ' + CFG }));
t('warning prefix keeps the tag', () => assert.deepStrictEqual(c.parseLine('W (99) test: error unknown command'), { tag: 'test', text: 'error unknown command' }));
t('bare tag', () => assert.deepStrictEqual(c.parseLine('test: ok color #ff8000'), { tag: 'test', text: 'ok color #ff8000' }));
t('other tags pass through', () => assert.deepStrictEqual(c.parseLine('I (5) mesh: hold on'), { tag: 'mesh', text: 'hold on' }));
t('colour codes and CR stripped', () => assert.deepStrictEqual(c.parseLine('\x1b[0;32mI (7) ble: advertising\x1b[0m\r'), { tag: 'ble', text: 'advertising' }));
t('no tag', () => assert.deepStrictEqual(c.parseLine('ESP-ROM:esp32s3-20210327'), { tag: null, text: 'ESP-ROM:esp32s3-20210327' }));
t('empty line', () => assert.strictEqual(c.parseLine('   \r'), null));
t('echo line is not a tag', () => assert.deepStrictEqual(c.parseLine('> cfg'), { tag: null, text: '> cfg' }));

t('cfg JSON', () => {
  const o = c.extractCfg('cfg ' + CFG);
  assert.strictEqual(o.color, '#ff8000'); assert.strictEqual(o.dev, false); assert.strictEqual(o.bright, 80);
  assert.strictEqual(o.version, 'v0.1.0-rc.0'); assert.strictEqual(o.addr, 'aa:bb:cc:dd:ee:ff');
  assert.deepStrictEqual(o.peers, []);
});
t('cfg with peers', () => {
  const o = c.extractCfg('cfg {"color":"#0000ff","dev":true,"name":"Léa","bright":50,"version":"v0.1.0-rc.1","addr":"11:22:33:44:55:66","peers":[{"addr":"aa:bb:cc:dd:ee:ff","color":"#ff0000","holding":true,"ver":"v0.1.0-rc.0"}]}');
  assert.strictEqual(o.name, 'Léa'); assert.strictEqual(o.peers.length, 1); assert.strictEqual(o.peers[0].holding, true);
});
t('cfg with a chosen wave colour', () => {
  const o = c.extractCfg('cfg {"color":"#ff8000","color2":"#00ff00","dev":true,"name":"","bright":80,"version":"v0.2.0-1-gcolor2","addr":"aa:bb:cc:dd:ee:ff","peers":[]}');
  assert.strictEqual(o.color2, '#00ff00'); assert.deepStrictEqual(c.color2State(o.color2), { auto: false, hex: '00ff00' });
});
t('cfg with an automatic wave colour', () => assert.deepStrictEqual(c.color2State(c.extractCfg('cfg ' + CFG).color2), { auto: true }));
t('cfg of an older firmware: no wave colour', () => {
  assert.strictEqual(c.color2State(undefined), null); assert.strictEqual(c.color2State(null), null);
  assert.strictEqual(c.color2State('garbage'), null); assert.strictEqual(c.color2State('#12345'), null);
});
t('wave colour state forms', () => assert.deepStrictEqual(c.color2State('#00FF80'), { auto: false, hex: '00ff80' }));
t('wave colour commands', () => {
  assert.strictEqual(c.color2Command(true), 'color2 auto'); assert.strictEqual(c.color2Command(true, 'ff0000'), 'color2 auto');
  assert.strictEqual(c.color2Command(false, '#00FF00'), 'color2 00ff00'); assert.strictEqual(c.color2Command(false, 'ff8000'), 'color2 ff8000');
  assert.strictEqual(c.color2Command(false, 'nope'), null); assert.strictEqual(c.color2Command(false), null);
});
t('the ok answers parse', () => {
  assert.deepStrictEqual(c.parseLine('I (9) test: ok color2 #00ff00'), { tag: 'test', text: 'ok color2 #00ff00' });
  assert.deepStrictEqual(c.parseLine('I (9) test: ok color2 auto'), { tag: 'test', text: 'ok color2 auto' });
  assert.deepStrictEqual(c.parseLine('I (9) cfg: color2 auto'), { tag: 'cfg', text: 'color2 auto' });
});
t('the automatic wave colour is the opposite hue (wave_opposite)', () => {
  assert.strictEqual(c.oppositeHex('#ff8000'), '007fff'); assert.strictEqual(c.oppositeHex('ff0000'), '00ffff');
  assert.strictEqual(c.oppositeHex('00ff00'), 'ff00ff'); assert.strictEqual(c.oppositeHex('808080'), '808080');
  assert.strictEqual(c.oppositeHex('c86432'), '3296c8'); assert.strictEqual(c.oppositeHex('xyz'), null);
});
t('cfg with trailing text', () => assert.strictEqual(c.extractCfg('cfg {"a":1} ').a, 1));
t('not a cfg line', () => assert.strictEqual(c.extractCfg('ok color #ff8000'), null));
t('broken JSON', () => assert.strictEqual(c.extractCfg('cfg {"color":'), null));
t('array is not a cfg', () => assert.strictEqual(c.extractCfg('cfg []'), null));
t('cfg without brace', () => assert.strictEqual(c.extractCfg('cfg'), null));

t('split keeps the partial line', () => assert.deepStrictEqual(c.splitLines('I (1) te', 'st: pong v1\r\nI (2) test: ok\nrest'),
  { lines: ['I (1) test: pong v1', 'I (2) test: ok'], rest: 'rest' }));
t('split with nothing complete', () => assert.deepStrictEqual(c.splitLines('', 'abc'), { lines: [], rest: 'abc' }));
t('split ending on a newline', () => assert.deepStrictEqual(c.splitLines('', 'a\n'), { lines: ['a'], rest: '' }));

t('hexColor forms', () => { assert.strictEqual(c.hexColor('#FF8000'), 'ff8000'); assert.strictEqual(c.hexColor('ff8000'), 'ff8000'); });
t('hexColor rejects', () => { assert.strictEqual(c.hexColor('zz'), null); assert.strictEqual(c.hexColor('#fff'), null); assert.strictEqual(c.hexColor(undefined), null); });
t('presets: 12 hues every 30 degrees', () => {
  assert.strictEqual(c.presetHex(0), 'ff0000'); assert.strictEqual(c.presetHex(1), 'ff8000'); assert.strictEqual(c.presetHex(2), 'ffff00');
  assert.strictEqual(c.presetHex(4), '00ff00'); assert.strictEqual(c.presetHex(6), '00ffff'); assert.strictEqual(c.presetHex(8), '0000ff');
  assert.strictEqual(c.presetHex(10), 'ff00ff'); assert.strictEqual(c.presetHex(11), 'ff0080'); assert.strictEqual(c.presetHex(12), 'ff0000');
  assert.strictEqual(new Set(Array.from({ length: 12 }, (_, i) => c.presetHex(i))).size, 12);
});

if (!process.exitCode) console.log('config.test.js: ' + n + ' checks passed');

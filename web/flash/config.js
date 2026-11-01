// Configure over USB: the flasher page's card talks to a ColorMesh board through Web Serial (Chrome / Edge on a
// computer, https or localhost) at 115200 baud, using the firmware's USB console (docs/PROTOCOL.md §2): `cfg`
// answers one JSON line `test: cfg {...}`; `color RRGGBB`, `color2 RRGGBB|auto` (the waves' second colour),
// `name <text>`, `devmode on|off` and `bright N` set the values and answer `test: ok ...` or `test: error ...`;
// secure updates: `admin on|off` (this board signs its firmware for boards out of dev mode), `trust <key>` /
// `trust none` (the admin key a board accepts; cfg's "admin", "admin_key", "trusted"). The console's answers are ESP-IDF log lines
// (`I (1234) test: ...`), so the parser tolerates that prefix, colour codes and CRLF. The pure functions are
// exported for web/flash/config.test.js (Node, no dependencies); the DOM part runs only in a browser.
'use strict';

// "I (1234) test: cfg {...}" -> {tag: "test", text: "cfg {...}"}; "test: ok" too; "plain" -> {tag: null, text}
const TAG_RE = /^(?:[IWEDV] \(\d+\) )?([a-z_]+): (.*)$/;

function parseLine(raw) {
  const line = String(raw).replace(/\x1b\[[0-9;]*m/g, '').replace(/\r$/, '').trim();
  if (!line) return null;
  const m = TAG_RE.exec(line);
  return m ? { tag: m[1], text: m[2] } : { tag: null, text: line };
}

// The JSON object of a "cfg {...}" answer (the text after the tag), or null
function extractCfg(text) {
  if (!/^cfg\s+\{/.test(text)) return null;
  const start = text.indexOf('{'), end = text.lastIndexOf('}');
  if (start < 0 || end <= start) return null;
  let obj;
  try { obj = JSON.parse(text.slice(start, end + 1)); } catch (e) { return null; }
  return obj && typeof obj === 'object' && !Array.isArray(obj) ? obj : null;
}

// Whole lines out of a stream: the partial last line comes back as `rest` for the next chunk
function splitLines(rest, chunk) {
  const parts = (rest + chunk).split(/\r?\n/);
  return { lines: parts.slice(0, -1), rest: parts[parts.length - 1] };
}

// "#FF8000" or "ff8000" -> "ff8000"; anything else null
function hexColor(v) {
  const m = /^#?([0-9a-fA-F]{6})$/.exec(String(v).trim());
  return m ? m[1].toLowerCase() : null;
}

// The 12 preset swatches: full-saturation hues every 30 degrees, as the board's pick screen
function presetHex(i) {
  const h = ((i % 12) + 12) % 12 * 30, x = 1 - Math.abs((h / 60) % 2 - 1);
  const rgb = h < 60 ? [1, x, 0] : h < 120 ? [x, 1, 0] : h < 180 ? [0, 1, x] : h < 240 ? [0, x, 1]
            : h < 300 ? [x, 0, 1] : [1, 0, x];
  return rgb.map(v => Math.round(v * 255).toString(16).padStart(2, '0')).join('');
}

// The waves' second colour from cfg's "color2": "auto" -> {auto: true}; "#rrggbb" -> {auto: false, hex}; anything
// else (an older firmware without it) -> null
function color2State(v) {
  if (v === 'auto') return { auto: true };
  const h = hexColor(v === undefined || v === null ? '' : v);
  return h ? { auto: false, hex: h } : null;
}

// The console command for the wave colour: automatic -> "color2 auto"; else "color2 rrggbb" (null for a bad colour)
function color2Command(auto, hex) {
  if (auto) return 'color2 auto';
  const h = hexColor(hex === undefined || hex === null ? '' : hex);
  return h ? 'color2 ' + h : null;
}

// The automatic wave colour: the opposite hue with the same saturation and value (the firmware's wave_opposite:
// each channel c -> max + min - c); null for a bad colour
function oppositeHex(hex) {
  const h = hexColor(hex === undefined || hex === null ? '' : hex);
  if (!h) return null;
  const c = [0, 2, 4].map(i => parseInt(h.slice(i, i + 2), 16)), s = Math.max(...c) + Math.min(...c);
  return c.map(v => (s - v).toString(16).padStart(2, '0')).join('');
}

// Secure updates from cfg: {admin, adminKey, trusted} (adminKey: the board's own public key while it is the admin,
// trusted: the short id, the first 8 characters, of the key it trusts), or null for a firmware without them
function secureState(c) {
  if (!c || typeof c.admin !== 'boolean' || !('trusted' in c)) return null;
  const key = typeof c.admin_key === 'string' && isAdminKey(c.admin_key) ? c.admin_key : null;
  const t = typeof c.trusted === 'string' && /^[A-Za-z0-9+/]{8}$/.test(c.trusted) ? c.trusted : null;
  return { admin: c.admin, adminKey: c.admin ? key : null, trusted: t };
}

// An admin public key as `admin key` prints it: 64 bytes in base64, 88 characters ending in "=="
function isAdminKey(s) { return /^[A-Za-z0-9+/]{86}==$/.test(String(s)); }

// The console command for a pasted key: spaces and line breaks taken out (a key copied from a wrapped line);
// "none" (or an empty field) -> "trust none"; anything that is not a key -> null
function trustCommand(v) {
  const k = String(v === undefined || v === null ? '' : v).replace(/\s+/g, '');
  if (k === '' || k === 'none') return 'trust none';
  return isAdminKey(k) ? 'trust ' + k : null;
}

function adminCommand(on) { return on ? 'admin on' : 'admin off'; }

// The short id of a key (what the board shows in its log lines and cfg's "trusted"): its first 8 characters
function shortId(key) { return isAdminKey(key) ? key.slice(0, 8) : null; }

if (typeof module !== 'undefined' && module.exports) {
  module.exports = { parseLine, extractCfg, splitLines, hexColor, presetHex, color2State, color2Command, oppositeHex,
                     secureState, isAdminKey, trustCommand, adminCommand, shortId };
}

if (typeof document !== 'undefined') initConfigCard();

function initConfigCard() {
  const el = id => document.getElementById(id);
  const card = el('cfg-card');
  if (!card) return;
  const status = el('cfg-status'), panel = el('cfg-panel'), logEl = el('cfg-log');
  const btnConnect = el('cfg-connect'), btnDisconnect = el('cfg-disconnect'), btnRefresh = el('cfg-refresh');
  let port = null, reader = null, readDone = null, closing = false, rest = '', cfgTimer = null;
  const log = [];

  function esc(s) { return String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]); }
  function say(msg, warn) { status.textContent = msg; status.classList.toggle('warn', !!warn); status.classList.toggle('muted', !warn); }
  function addLog(line) {
    log.push(line);
    if (log.length > 20) log.shift();
    logEl.hidden = false;
    logEl.textContent = log.join('\n');
    logEl.scrollTop = logEl.scrollHeight;
  }

  if (!('serial' in navigator)) {
    say(window.isSecureContext
      ? "This browser can't talk to USB devices: use Chrome or Edge on a computer."
      : 'USB access needs a secure page (https or localhost).', true);
    btnConnect.disabled = true;
    return;
  }

  function addSwatches(box, label, pick) {
    for (let i = 0; i < 12; i++) {
      const h = presetHex(i), b = document.createElement('button');
      b.type = 'button'; b.className = 'swatch'; b.dataset.hex = h;
      b.style.background = '#' + h; b.title = '#' + h; b.setAttribute('aria-label', label + ' #' + h);
      b.onclick = () => pick(h);
      box.appendChild(b);
    }
  }
  const sw = el('cfg-swatches'), sw2 = el('cfg-swatches2');
  addSwatches(sw, 'Colour', h => setColor(h));
  addSwatches(sw2, 'Wave colour', h => setColor2(h));
  function markIn(box, h) { for (const b of box.children) b.setAttribute('aria-pressed', String(b.dataset.hex === h)); }
  function markSwatch(h) { markIn(sw, h); }
  // Automatic: the picker shows the opposite of the colour, greyed; unchecked: a pick sends "color2 RRGGBB"
  let color2Auto = true;
  function showColor2(auto, hex) {
    color2Auto = auto;
    el('cfg-color2-auto').checked = auto;
    el('cfg-color2').disabled = auto;
    for (const b of sw2.children) b.disabled = auto;
    el('cfg-color2-row').classList.toggle('off', auto);
    const h = auto ? oppositeHex(el('cfg-color').value) : hexColor(hex || '');
    if (h) el('cfg-color2').value = '#' + h;
    markIn(sw2, auto ? null : h);
    el('cfg-color2-note').textContent = auto ? 'the opposite of the colour' : (hex ? '' : 'pick one');
  }

  async function send(cmd) {
    if (!port || !port.writable) { say('Not connected.', true); return; }
    const w = port.writable.getWriter();
    try { await w.write(new TextEncoder().encode(cmd + '\n')); addLog('> ' + cmd); }
    catch (e) { say('Could not write to the board: ' + e.message, true); }
    finally { w.releaseLock(); }
  }
  function refreshSoon() { clearTimeout(cfgTimer); cfgTimer = setTimeout(() => send('cfg'), 300); }
  function setColor(h) {
    el('cfg-color').value = '#' + h; markSwatch(h); send('color ' + h); refreshSoon();
    if (color2Auto) showColor2(true);
  }
  function setColor2(h) {
    const cmd = color2Command(false, h);
    if (!cmd) return;
    showColor2(false, h); send(cmd); refreshSoon();
  }

  function render(c) {
    panel.hidden = false;
    const h = hexColor(c.color);
    if (h) { el('cfg-color').value = '#' + h; markSwatch(h); }
    const c2 = color2State(c.color2);
    el('cfg-color2-row').hidden = el('cfg-color2-auto-row').hidden = !c2;   // an older firmware: no wave colour
    if (c2) showColor2(c2.auto, c2.hex);
    if (document.activeElement !== el('cfg-name')) el('cfg-name').value = c.name || '';
    el('cfg-dev').checked = !!c.dev;
    const b = Number.isFinite(c.bright) ? Math.max(0, Math.min(100, c.bright)) : 100;
    el('cfg-bright').value = b; el('cfg-bright-v').textContent = b + ' %';
    const sec = secureState(c);
    el('cfg-secure').hidden = !sec;                  // an older firmware: no secure updates
    if (sec) {
      el('cfg-admin').checked = sec.admin;
      el('cfg-admin-box').hidden = !sec.adminKey;
      el('cfg-admin-key').textContent = sec.adminKey || '';
      el('cfg-trust-now').textContent = sec.trusted ? 'trusts the admin key ' + sec.trusted + '…' : 'trusts no admin key';
    }
    el('cfg-version').textContent = c.version || '?';
    el('cfg-addr').textContent = c.addr || '?';
    const peers = Array.isArray(c.peers) ? c.peers : [];
    el('cfg-peers').innerHTML = peers.length ? peers.map(p =>
      `<li><span class="dot" style="background:#${hexColor(p.color) || '000000'}"></span>${esc(p.addr || '?')}` +
      ` · v${esc(String(p.ver || '?').replace(/^v/, ''))} · ${p.holding ? 'holding' : 'idle'}</li>`).join('')
      : '<li class="muted">none</li>';
    say('Connected: ' + (c.name ? c.name + ' · ' : '') + (c.version || 'ColorMesh') + (c.dev ? ' · dev mode' : '') +
        (sec && sec.admin ? ' · admin' : ''));
  }

  function onLine(raw) {
    const p = parseLine(raw);
    if (!p) return;
    addLog(p.tag ? p.tag + ': ' + p.text : p.text);
    if (p.tag !== 'test') return;
    const c = extractCfg(p.text);
    if (c) { render(c); return; }
    if (/^error\b/.test(p.text)) say('The board said: ' + p.text, true);
  }

  async function readLoop() {
    const dec = new TextDecoder();
    while (port && port.readable && !closing) {
      reader = port.readable.getReader();
      try {
        for (;;) {
          const { value, done } = await reader.read();
          if (done) break;
          const r = splitLines(rest, dec.decode(value, { stream: true }));
          rest = r.rest;
          r.lines.forEach(onLine);
        }
      } catch (e) {
        if (!closing) addLog('(read error: ' + e.message + ')');
      } finally {
        reader.releaseLock();
        reader = null;
      }
    }
  }

  function uiDisconnected(msg, warn) {
    port = null; readDone = null; closing = false; rest = '';
    btnConnect.hidden = false; btnDisconnect.hidden = true; btnRefresh.hidden = true; panel.hidden = true;
    say(msg || 'Not connected.', warn);
  }

  async function connect() {
    let p;
    try { p = await navigator.serial.requestPort(); }
    catch (e) { say('No port chosen.'); return; }
    try { await p.open({ baudRate: 115200 }); }
    catch (e) {
      if (e.name === 'InvalidStateError') say('That port is already open in this page.', true);
      else say('Could not open the port (' + e.message + '). If the Install dialog is open, or another program ' +
               'reads the port (a serial monitor, the flash helper), close it and try again.', true);
      return;
    }
    port = p; closing = false; rest = '';
    btnConnect.hidden = true; btnDisconnect.hidden = false; btnRefresh.hidden = false;
    say('Connected, reading the configuration…');
    readDone = readLoop().then(() => { if (port) uiDisconnected('The board went away (unplugged, or restarting).', true); });
    await send('cfg');
    setTimeout(() => { if (port && panel.hidden) say('No answer yet: is the board running ColorMesh? Press Refresh.', true); }, 2000);
  }

  async function disconnect() {
    if (!port) return;
    closing = true;
    try { if (reader) await reader.cancel(); } catch (e) {}
    try { if (readDone) await readDone; } catch (e) {}
    try { await port.close(); } catch (e) {}
    uiDisconnected('Disconnected.');
  }

  btnConnect.onclick = connect;
  btnDisconnect.onclick = disconnect;
  btnRefresh.onclick = () => send('cfg');
  el('cfg-color').onchange = ev => { const h = hexColor(ev.target.value); if (h) setColor(h); };
  el('cfg-color2').onchange = ev => { const h = hexColor(ev.target.value); if (h) setColor2(h); };
  el('cfg-color2-auto').onchange = ev => {
    if (ev.target.checked) { showColor2(true); send(color2Command(true)); refreshSoon(); }
    else showColor2(false);                          // nothing sent until a colour is picked
  };
  el('cfg-name-set').onclick = () => { send('name ' + el('cfg-name').value.trim().slice(0, 16)); refreshSoon(); };
  el('cfg-name').onkeydown = ev => { if (ev.key === 'Enter') el('cfg-name-set').click(); };
  el('cfg-dev').onchange = ev => { send('devmode ' + (ev.target.checked ? 'on' : 'off')); refreshSoon(); };
  el('cfg-admin').onchange = ev => { send(adminCommand(ev.target.checked)); refreshSoon(); };
  el('cfg-admin-copy').onclick = async () => {
    const k = el('cfg-admin-key').textContent;
    try { await navigator.clipboard.writeText(k); say('Admin key copied: paste it into the other boards\' "Trusted admin key".'); }
    catch (e) { say('Could not copy: select the key and copy it by hand.', true); }
  };
  el('cfg-trust-set').onclick = () => {
    const cmd = trustCommand(el('cfg-trust').value);
    if (!cmd || cmd === 'trust none') { say('Paste the admin board\'s key: 88 characters ending in "==".', true); return; }
    send(cmd); el('cfg-trust').value = ''; refreshSoon();
  };
  el('cfg-trust').onkeydown = ev => { if (ev.key === 'Enter') el('cfg-trust-set').click(); };
  el('cfg-trust-forget').onclick = () => { send('trust none'); refreshSoon(); };
  el('cfg-bright').oninput = ev => { el('cfg-bright-v').textContent = ev.target.value + ' %'; };
  el('cfg-bright').onchange = ev => { send('bright ' + ev.target.value); refreshSoon(); };
  window.addEventListener('beforeunload', () => { if (port) { try { port.close(); } catch (e) {} } });
}

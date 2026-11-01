// Configure over USB: the flasher page's card talks to a ColorMesh board through Web Serial (Chrome / Edge on a
// computer, https or localhost) at 115200 baud, using the firmware's USB console (docs/PROTOCOL.md §2): `cfg`
// answers one JSON line `test: cfg {...}`; `color RRGGBB`, `name <text>`, `devmode on|off` and `bright N` set the
// values and answer `test: ok ...` or `test: error ...`. The console's answers are ESP-IDF log lines
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

if (typeof module !== 'undefined' && module.exports) {
  module.exports = { parseLine, extractCfg, splitLines, hexColor, presetHex };
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

  const sw = el('cfg-swatches');
  for (let i = 0; i < 12; i++) {
    const h = presetHex(i), b = document.createElement('button');
    b.type = 'button'; b.className = 'swatch'; b.dataset.hex = h;
    b.style.background = '#' + h; b.title = '#' + h; b.setAttribute('aria-label', 'Colour #' + h);
    b.onclick = () => setColor(h);
    sw.appendChild(b);
  }
  function markSwatch(h) { for (const b of sw.children) b.setAttribute('aria-pressed', String(b.dataset.hex === h)); }

  async function send(cmd) {
    if (!port || !port.writable) { say('Not connected.', true); return; }
    const w = port.writable.getWriter();
    try { await w.write(new TextEncoder().encode(cmd + '\n')); addLog('> ' + cmd); }
    catch (e) { say('Could not write to the board: ' + e.message, true); }
    finally { w.releaseLock(); }
  }
  function refreshSoon() { clearTimeout(cfgTimer); cfgTimer = setTimeout(() => send('cfg'), 300); }
  function setColor(h) { el('cfg-color').value = '#' + h; markSwatch(h); send('color ' + h); refreshSoon(); }

  function render(c) {
    panel.hidden = false;
    const h = hexColor(c.color);
    if (h) { el('cfg-color').value = '#' + h; markSwatch(h); }
    if (document.activeElement !== el('cfg-name')) el('cfg-name').value = c.name || '';
    el('cfg-dev').checked = !!c.dev;
    const b = Number.isFinite(c.bright) ? Math.max(0, Math.min(100, c.bright)) : 100;
    el('cfg-bright').value = b; el('cfg-bright-v').textContent = b + ' %';
    el('cfg-version').textContent = c.version || '?';
    el('cfg-addr').textContent = c.addr || '?';
    const peers = Array.isArray(c.peers) ? c.peers : [];
    el('cfg-peers').innerHTML = peers.length ? peers.map(p =>
      `<li><span class="dot" style="background:#${hexColor(p.color) || '000000'}"></span>${esc(p.addr || '?')}` +
      ` · v${esc(String(p.ver || '?').replace(/^v/, ''))} · ${p.holding ? 'holding' : 'idle'}</li>`).join('')
      : '<li class="muted">none</li>';
    say('Connected: ' + (c.name ? c.name + ' · ' : '') + (c.version || 'ColorMesh') + (c.dev ? ' · dev mode' : ''));
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
  el('cfg-name-set').onclick = () => { send('name ' + el('cfg-name').value.trim().slice(0, 16)); refreshSoon(); };
  el('cfg-name').onkeydown = ev => { if (ev.key === 'Enter') el('cfg-name-set').click(); };
  el('cfg-dev').onchange = ev => { send('devmode ' + (ev.target.checked ? 'on' : 'off')); refreshSoon(); };
  el('cfg-bright').oninput = ev => { el('cfg-bright-v').textContent = ev.target.value + ' %'; };
  el('cfg-bright').onchange = ev => { send('bright ' + ev.target.value); refreshSoon(); };
  window.addEventListener('beforeunload', () => { if (port) { try { port.close(); } catch (e) {} } });
}

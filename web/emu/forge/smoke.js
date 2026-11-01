// Smoke test of the display in the browser (README.md): serves web/emu/build, opens it in headless Chromium and
// checks, with real mouse events, that it starts, draws, follows a drag and a press and hold, that its settings
// page talks to it (a language change on the page changes the screen) and that screen dimming's /api/presence answers
// (forge_presence's real presence.c runs here). Any page or console error fails it.
//   node web/emu/forge/smoke.js [build dir] [screenshots dir]      (Playwright from tools/webtest: npm ci there;
//   from an app's .espforge copy: cd tools/webtest && node ../../.espforge/web/emu/forge/smoke.js ../../web/emu/build)
// Screenshots of the screen at each step: web/emu/build/smoke/ by default. Exit code 0 = passed.
const http = require('http');
const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..', '..', '..');
// Playwright from this repository's tools/webtest, else from where it is run: an app that takes espforge at a tag runs
// this file from its .espforge copy, which has no node_modules (esp32-s3-rtcquebec: cd tools/webtest first)
const { chromium } = require(require.resolve('playwright',
  { paths: [path.join(root, 'tools', 'webtest'), process.cwd()] }));
const dir = path.resolve(process.argv[2] || path.join(__dirname, '..', 'build'));
const shots = path.resolve(process.argv[3] || path.join(dir, 'smoke'));
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.ttf': 'font/ttf' };

function serve() {                                 // build/ as the site's root (index.html's ../fonts/ is fonts/)
  const s = http.createServer((req, res) => {
    const f = path.join(dir, decodeURIComponent(new URL(req.url, 'http://x').pathname).replace(/\/$/, '/index.html'));
    if (!f.startsWith(dir) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'Content-Type': TYPES[path.extname(f)] || 'application/octet-stream' });
    fs.createReadStream(f).pipe(res);
  });
  return new Promise(ok => s.listen(0, '127.0.0.1', () => ok(s)));
}

(async () => {
  fs.mkdirSync(shots, { recursive: true });
  const server = await serve();
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 1100, height: 900 } });
  const errors = [];
  page.on('pageerror', e => errors.push('page error: ' + e.message));
  const logs = [];                                 // the firmware's log lines (ESP_LOGx goes to the console)
  page.on('console', m => { logs.push(m.text()); if (m.type() === 'error') errors.push('console: ' + m.text()); });
  let failed = 0;
  const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failed++; };
  // The screen as the page shows it: a hash of the canvas, and a screenshot
  const screen = page.locator('#screen');
  const look = async name => {
    await page.waitForTimeout(1500);              // moves settle (slide.c), LVGL redraws
    await screen.screenshot({ path: path.join(shots, name + '.png') });
    return page.evaluate(() => {
      const c = document.getElementById('screen'), d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
      let h = 0, lit = 0;
      for (let i = 0; i < d.length; i += 4) { h = (h * 31 + d[i] + d[i + 1] * 7 + d[i + 2] * 13) | 0; if (d[i] + d[i + 1] + d[i + 2] > 60) lit++; }
      return { h, lit };
    });
  };
  try {
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    await page.waitForFunction(() => document.getElementById('status').textContent === '', null, { timeout: 30000 });
    const start = await look('1-start');
    check(start.lit > 500, `the screen draws (${start.lit} lit pixels)`);

    const b = await screen.boundingBox(), cx = b.x + b.width / 2, cy = b.y + b.height / 2;
    await page.mouse.move(cx + b.width * 0.3, cy);
    await page.mouse.down();
    for (let i = 1; i <= 15; i++) { await page.mouse.move(cx + b.width * (0.3 - 0.04 * i), cy); await page.waitForTimeout(20); }
    await page.mouse.up();
    const swiped = await look('2-swiped');
    check(swiped.h !== start.h && swiped.lit > 500, 'a drag to the left shows another page');

    await page.mouse.move(cx, cy);
    await page.mouse.down();
    await page.waitForTimeout(1500);
    await page.mouse.up();
    const held = await look('3-held');
    check(held.h !== swiped.h, 'a press and hold changes the screen');

    // The settings page: its /api/ requests are served by the firmware's handlers (emu_web.c)
    const frame = page.frameLocator('#settings');
    const langs = frame.locator('#lang option');
    await langs.nth(1).waitFor({ state: 'attached', timeout: 15000 });
    check(await langs.count() > 1, 'the settings page reads /api/info (the languages)');
    // Screen dimming's route, served by forge_presence's real presence.c (its task runs here, without microphones)
    const pres = await page.evaluate(() => window.emuApi('GET', '/api/presence', ''));
    let pj = null;
    try { pj = JSON.parse(pres.body); } catch (e) { /* not JSON: said below */ }
    check(pj && typeof pj.state === 'string',
      `/api/presence answers with "state" (${pres.status}: ${String(pres.body).slice(0, 60)})`);
    check(logs.some(t => /presence: microphones/.test(t)), "forge_presence's task started (its microphones line in the log)");
    await page.reload();                           // back to the first screen, then change the language there
    await page.waitForFunction(() => document.getElementById('status').textContent === '', null, { timeout: 30000 });
    const before = await look('4-before-language');
    const other = await langs.nth(1).getAttribute('value');
    await frame.locator('#lang').selectOption(other);
    const after = await look('5-language-' + other);
    check(after.h !== before.h, `choosing "${other}" on the settings page changes the screen`);
    // The language is kept (NVS in localStorage): back to the first one for the next run
    await frame.locator('#lang').selectOption(await langs.nth(0).getAttribute('value'));
    await page.waitForTimeout(500);
  } catch (e) {
    check(false, 'ran to the end: ' + e.message.split('\n')[0]);
  }
  for (const e of errors) check(false, e);
  if (!errors.length) check(true, 'no page or console errors');
  console.log(`screenshots in ${shots}`);
  await browser.close();
  server.close();
  process.exit(failed ? 1 : 0);
})();

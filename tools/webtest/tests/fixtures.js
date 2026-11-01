// Shared test setup: fresh mock state, no internet (the page must not need it: on the setup network there is none),
// page errors collected, a screenshot of each test in shots/.
// The page's contract (main/web/index.html): element ids #wifi-list #wifi-ssid #wifi-pass #wifi-save #lang
// #update-state #update-check #update-install #update-channel #update-notes #msg #version, screen dimming's #scr-state
// #scr-level #scr-en #scr-dim #scr-off #scr-bright #scr-dimpct #scr-motion #scr-save #scr-cal; texts through t(key) and
// I18N = {en: {...}, fr: {...}}, elements with data-i18n="key"; the key from location.hash "#k=<key>", kept in
// sessionStorage.
const base = require('@playwright/test');
const fs = require('fs');
const path = require('path');
const http = require('http');
const { spawn } = require('child_process');

const KEY = process.env.MOCK_KEY || '0123456789abcdef';   // the same as mock-server.js

// Each worker's own mock display (mock-server.js) on port 8100 + its index: parallel workers never share its state.
// 127.0.0.1, not localhost: Node may resolve localhost to ::1 while the mock listens on IPv4 (ECONNREFUSED ::1).
const up = url => new Promise(done => http.get(url, r => { r.resume(); done(r.statusCode === 200); })
  .on('error', () => done(false)));

exports.test = base.test.extend({
  mockPort: [async ({}, use, workerInfo) => {
    const port = 8100 + workerInfo.parallelIndex;
    const mock = spawn(process.execPath, [path.join(__dirname, '..', 'mock-server.js'), String(port)], { stdio: 'ignore' });
    const until = Date.now() + 10000;
    while (!(await up(`http://127.0.0.1:${port}/api/info`))) {
      if (Date.now() > until) throw new Error(`mock-server.js didn't answer on port ${port}`);
      await new Promise(r => setTimeout(r, 100));
    }
    await use(port);
    mock.kill();
  }, { scope: 'worker' }],
  baseURL: async ({ mockPort }, use) => use(`http://127.0.0.1:${mockPort}`),
  page: async ({ page, request }, use, testInfo) => {
    await request.post('/__reset');
    const errors = [];
    page.on('pageerror', e => errors.push(e.message));
    // (a 401 is the display refusing a change without its key: the page handles it, the browser still logs it)
    page.on('console', m => { if (m.type() === 'error' && !/status of 40[01]/.test(m.text())) errors.push(m.text()); });
    page.on('dialog', d => d.accept());           // confirm() before an install or a Wi-Fi change: yes
    await page.route(/^https?:\/\/(?!localhost|127\.0\.0\.1)/, r => r.abort('internetdisconnected'));
    await use(page);
    fs.mkdirSync(path.join(__dirname, '..', 'shots'), { recursive: true });
    await page.screenshot({ path: path.join(__dirname, '..', 'shots', testInfo.title.replace(/[^a-z0-9]+/gi, '_') + '.png'),
                            fullPage: true });
    base.expect(errors, 'errors in the page').toEqual([]);
  },
});
exports.expect = base.expect;
exports.KEY = KEY;

// The mock's state (what the page sent to the "display")
exports.state = async request => (await request.get('/__state')).json();
// The page with the display's key, as the settings QR code opens it
exports.open = (page, key = KEY) => page.goto(key ? '/#k=' + key : '/');
// The page's own translation table (a global script binding, not necessarily a window property)
exports.table = page => page.evaluate(() => I18N);
// What the page shows for each data-i18n element (inputs and selects left out: their text is not the label)
exports.labels = page => page.evaluate(() => [...document.querySelectorAll('[data-i18n]')]
  .filter(e => !['INPUT', 'TEXTAREA', 'SELECT'].includes(e.tagName))
  .map(e => ({ key: e.getAttribute('data-i18n'), text: e.textContent.trim() })));
exports.posts = async (request, url) =>
  (await exports.state(request)).log.filter(e => e.method === 'POST' && e.url === url);

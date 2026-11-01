// Firmware card: every updater state renders (GET /api/update, docs/PROTOCOL.md §4), Install only when an update is
// offered. In the source project a JavaScript parameter named `t` hid the page's translation function t(): the Install
// button never showed, in two releases, because nothing tested the "available" state.
const { test, expect, state, open, posts } = require('./fixtures');

const NOTES = 'v0.2.0|October 4, 2026\nA new screen.\nFaster start-up.\n\nv0.1.1|October 1, 2026\nA fix.\n';
const STATES = {
  idle: {},
  checking: { check_result: 'checking' },                // stays "checking" for the test
  up_to_date: { latest: 'v0.1.0-test' },
  available: { latest: 'v0.2.0', notes: NOTES },
  downloading: { latest: 'v0.2.0', progress: 42 },
  done: { latest: 'v0.2.0', progress: 100 },
  failed: { error: 'TLS handshake failed' },
};
const script = (request, name, extra = {}) =>
  request.post('/__update', { data: { state: name, ...STATES[name], ...extra } });

async function shown(page) {
  const s = page.locator('#update-state');
  await expect(s).not.toBeEmpty();
  const text = (await s.textContent()).trim();
  expect(text).not.toMatch(/undefined|\[object|NaN/);
  return text;
}

for (const name of Object.keys(STATES)) {
  test(`state ${name} renders, Install ${name === 'available' ? 'shown' : 'hidden'}`, async ({ page, request }) => {
    await script(request, name);
    await open(page);
    const text = await shown(page);
    if (name === 'available') {
      await expect(page.locator('#update-install')).toBeVisible();
      expect(text).toContain('v0.2.0');
      const notes = page.locator('#update-notes');
      await expect(notes).toBeVisible();
      await expect(notes).toContainText('A new screen.');
      await expect(notes).toContainText('v0.1.1');
    } else {
      await expect(page.locator('#update-install')).toBeHidden();
    }
    if (name === 'downloading') expect(text).toContain('42');
    if (name === 'failed') await expect(page.locator('#update-state, #msg').filter({ hasText: 'TLS handshake failed' }))
      .not.toHaveCount(0);
  });
}

test('each state reads differently', async ({ page, request }) => {
  const seen = {};
  for (const name of ['checking', 'up_to_date', 'available', 'downloading', 'failed']) {
    await script(request, name);
    await open(page);
    await page.reload();
    seen[name] = await shown(page);
  }
  expect(new Set(Object.values(seen)).size, JSON.stringify(seen)).toBe(Object.keys(seen).length);
});

test('a rollback is reported', async ({ page, request }) => {
  await script(request, 'idle');
  await open(page);
  const plain = await shown(page) + (await page.locator('#msg').textContent());
  await script(request, 'idle', { rolled_back: 'v0.2.0' });            // the version that was undone
  await page.reload();
  const rolled = await shown(page) + (await page.locator('#msg').textContent());
  expect(rolled).not.toBe(plain);
});

test('Install in French: the button still shows and t() is still a function', async ({ page, request }) => {
  await script(request, 'available');
  await open(page);
  await page.locator('#lang').selectOption('fr');
  await expect.poll(async () => (await state(request)).info.lang).toBe('fr');
  await expect(page.locator('#update-install')).toBeVisible();
  expect(await page.evaluate(() => typeof t)).toBe('function');
  const I18N = await page.evaluate(() => I18N);
  const text = await shown(page);
  expect(Object.values(I18N.en)).not.toContain(text);           // not left in English
});

test('Check asks the display, which answers up to date', async ({ page, request }) => {
  await open(page);
  await page.locator('#update-check').click();
  await expect.poll(async () => (await posts(request, '/api/update')).map(e => e.body)).toContainEqual({ action: 'check' });
  // the mock answers "checking" once, then up_to_date: the page polls until the check is over
  await expect.poll(async () => (await state(request)).update.state, { timeout: 10000 }).toBe('up_to_date');
  await shown(page);
});

test('Install sends install and the card follows the download', async ({ page, request }) => {
  await script(request, 'available');
  await open(page);
  await page.locator('#update-install').click();
  await expect.poll(async () => (await posts(request, '/api/update')).map(e => e.body)).toContainEqual({ action: 'install' });
  await expect(page.locator('#update-install')).toBeHidden({ timeout: 10000 });
});

test('the channel picker posts the channel', async ({ page, request }) => {
  await open(page);
  const ch = page.locator('#update-channel');
  if (await ch.evaluate(e => e.tagName) === 'SELECT') await ch.selectOption('beta');
  else await ch.click();
  await expect.poll(async () => (await state(request)).update.channel).toBe('beta');
  expect((await posts(request, '/api/update')).map(e => e.body)).toContainEqual({ channel: 'beta' });
});

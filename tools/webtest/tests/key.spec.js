// The display's key (forge_net web): every POST carries X-Key. The page gets it from the settings QR code (#k=...)
// and keeps it for the session; without it a change is refused and the page must say how to get the key.
const { test, expect, state, open, posts, KEY } = require('./fixtures');

test('the key from the QR code is kept for the session and sent with changes', async ({ page, request }) => {
  await open(page);
  await expect(page.locator('#version')).toContainText('v0.1.0-test');
  const kept = await page.evaluate(() => Object.keys(sessionStorage).map(k => sessionStorage.getItem(k)));
  expect(kept).toContain(KEY);
  await page.goto('/');                                           // later, without the code: still known
  await page.locator('#lang').selectOption('fr');
  await expect.poll(async () => (await state(request)).info.lang).toBe('fr');
  expect((await state(request)).log.filter(e => e.refused)).toEqual([]);
});

test('without the key a change is refused and the page says so clearly', async ({ page, request }) => {
  await open(page, null);
  await expect(page.locator('#version')).toContainText('v0.1.0-test');
  await page.locator('#lang').selectOption('fr');
  await expect.poll(async () => (await state(request)).log.some(e => e.refused === 401)).toBe(true);
  const msg = page.locator('#msg');
  await expect(msg).toBeVisible();
  await expect(msg).not.toBeEmpty();
  const text = (await msg.textContent()).trim();
  expect(text.length, `"${text}" is not an explanation`).toBeGreaterThan(15);
  expect(text).not.toMatch(/undefined|\[object/);
  expect((await state(request)).info.lang).toBe('en');
});

test('a refused Wi-Fi save does not claim success', async ({ page, request }) => {
  await open(page, null);
  await page.locator('#wifi-ssid').fill('HomeNet');
  await page.locator('#wifi-pass').fill('whatever-1');
  await page.locator('#wifi-save').click();
  await expect.poll(async () => (await state(request)).log.some(e => e.refused === 401)).toBe(true);
  expect((await state(request)).wifi).toBe(null);
  await expect(page.locator('#msg')).not.toBeEmpty();
});

test('on the setup network no key is needed', async ({ page, request }) => {
  await request.post('/__setup');
  await open(page, null);
  await page.locator('#wifi-ssid').fill('HomeNet');
  await page.locator('#wifi-pass').fill('home-pass-1');
  await page.locator('#wifi-save').click();
  await expect.poll(async () => (await state(request)).wifi).toEqual({ ssid: 'HomeNet', pass: 'home-pass-1' });
  expect((await posts(request, '/api/wifi')).length).toBe(1);
});

// MeteoBus's page said "Could not reach the display" when the display refused the key (after its address or port
// changed, the page's key was for another origin): the message must say what to do, scan the QR code again.
test('a refused key says to scan the QR code again, not that the display is unreachable', async ({ page, request }) => {
  await open(page, 'not-the-displays-key');
  await expect(page.locator('#version')).toContainText('v0.1.0-test');
  await page.locator('#lang').selectOption('fr');
  await expect.poll(async () => (await state(request)).log.some(e => e.refused === 401)).toBe(true);
  const text = (await page.locator('#msg').textContent()).trim();
  expect(text, `"${text}" doesn't say to scan the QR code`).toMatch(/QR/);
  expect(text).not.toMatch(/reach|didn't work|échec/i);
  expect((await state(request)).info.lang).toBe('en');
});

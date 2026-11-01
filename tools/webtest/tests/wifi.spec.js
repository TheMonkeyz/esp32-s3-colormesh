// Wi-Fi card: the networks the display sees, and saving one (POST /api/wifi {ssid, pass})
const { test, expect, state, open, posts } = require('./fixtures');

test('the scan list shows the networks the display sees', async ({ page }) => {
  await open(page);
  await expect(page.locator('#version')).toContainText('v0.1.0-test');
  const list = page.locator('#wifi-list');
  await expect(list).toContainText('HomeNet');
  await expect(list).toContainText('Cafe');
});

test('saving a network posts its name and password', async ({ page, request }) => {
  await open(page);
  await page.locator('#wifi-ssid').fill('Cafe');
  await page.locator('#wifi-pass').fill('secret-pass-1');
  await page.locator('#wifi-save').click();
  await expect.poll(async () => (await state(request)).wifi).toEqual({ ssid: 'Cafe', pass: 'secret-pass-1' });
  const p = await posts(request, '/api/wifi');
  expect(p.map(e => e.body)).toEqual([{ ssid: 'Cafe', pass: 'secret-pass-1' }]);
  await expect(page.locator('#msg')).not.toBeEmpty();                  // says what happens next (a restart)
});

test('an open network saves with an empty password', async ({ page, request }) => {
  await open(page);
  await page.locator('#wifi-ssid').fill('Cafe');
  await page.locator('#wifi-save').click();
  await expect.poll(async () => (await state(request)).wifi).toEqual({ ssid: 'Cafe', pass: '' });
});

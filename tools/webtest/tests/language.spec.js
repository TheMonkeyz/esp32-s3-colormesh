// Language: every text exists in both languages, the page follows the display's language and changes it
const { test, expect, state, open, posts, table, labels, KEY } = require('./fixtures');

test('every text exists in English and French', async ({ page }) => {
  await open(page);
  const I18N = await table(page);
  expect(Object.keys(I18N).sort()).toEqual(['en', 'fr']);
  for (const [a, b] of [['en', 'fr'], ['fr', 'en']])
    for (const k of Object.keys(I18N[a]))
      expect(I18N[b][k], `I18N.${b}.${k} (missing or empty)`).toBeTruthy();
  for (const { key } of await labels(page))
    expect(I18N.en[key], `data-i18n="${key}" has no text`).toBeTruthy();
});

test('switching to French changes the page and tells the display', async ({ page, request }) => {
  await open(page);
  await expect(page.locator('#version')).toContainText('v0.1.0-test');
  const I18N = await table(page);
  const en = await labels(page);
  expect(en.length, 'elements with data-i18n').toBeGreaterThan(0);
  for (const { key, text } of en) expect(text, key).toContain(I18N.en[key]);
  await page.locator('#lang').selectOption('fr');
  await expect.poll(async () => (await state(request)).info.lang).toBe('fr');
  expect((await posts(request, '/api/settings')).map(e => e.body)).toContainEqual({ lang: 'fr' });
  // The mock saves the language before the page has its answer: "saved" comes after the texts are applied (read
  // right after the mock's state, on macOS CI the labels were still English)
  await expect(page.locator('#msg')).toHaveText(I18N.fr.saved);
  const fr = await labels(page);
  for (const { key, text } of fr) expect(text, key).toContain(I18N.fr[key]);
  expect(fr.map(l => l.text)).not.toEqual(en.map(l => l.text));
  // t() is the page's translation function: it must still be one (a parameter named t once hid it)
  expect(await page.evaluate(k => t(k), en[0].key)).toBe(I18N.fr[en[0].key]);
});

test('the page opens in the display\'s language', async ({ page, request }) => {
  await request.post('/api/settings', { data: { lang: 'fr' }, headers: { 'X-Key': KEY } });
  await open(page);
  const I18N = await table(page);
  await expect(page.locator('#lang')).toHaveValue('fr');
  const fr = await labels(page);
  for (const { key, text } of fr) expect(text, key).toContain(I18N.fr[key]);
  await page.locator('#lang').selectOption('en');
  await expect.poll(async () => (await state(request)).info.lang).toBe('en');
});

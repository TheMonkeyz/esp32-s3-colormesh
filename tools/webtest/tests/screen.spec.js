// Screen dimming (forge_presence): the settings load and save, calibration shows its state and its verdict ("cal"),
// texts in both languages
const { test, expect, state, open, posts, table } = require('./fixtures');

test('screen settings load and save', async ({ page, request }) => {
  await request.post('/__presence', { data: { dim_s: 300, off_s: 1500, bright_pct: 80, dim_pct: 10, motion_wake: false } });
  await open(page);
  await expect(page.locator('#scr-state')).toHaveText('Active · brightness 100 %');
  await expect(page.locator('#scr-en')).toBeChecked();
  await expect(page.locator('#scr-dim')).toHaveValue('300');
  await expect(page.locator('#scr-dim option:checked')).toHaveText('5 min');
  await expect(page.locator('#scr-off')).toHaveValue('1800');           // dim + off: quiet in all
  await expect(page.locator('#scr-bright')).toHaveValue('80');
  await expect(page.locator('#scr-dimpct')).toHaveValue('10');
  await expect(page.locator('#scr-motion')).not.toBeChecked();

  await page.locator('#scr-en').uncheck();
  await page.locator('#scr-dim').selectOption('600');
  await page.locator('#scr-off').selectOption('7200');
  await page.locator('#scr-bright').fill('90');
  await page.locator('#scr-dimpct').fill('20');
  await page.locator('#scr-motion').check();
  await page.locator('#scr-save').click();
  await expect(page.locator('#msg')).toHaveText('Saved.');
  const sent = (await posts(request, '/api/presence')).map(e => e.body);
  expect(sent).toEqual([{ enabled: false, dim_s: 600, off_s: 6600, bright_pct: 90, dim_pct: 20, motion_wake: true }]);
  const p = (await state(request)).presence;
  expect([p.enabled, p.dim_s, p.off_s, p.bright_pct, p.dim_pct, p.motion_wake]).toEqual([false, 600, 6600, 90, 20, true]);
  expect(p.baseline_db).toBe(-60);                                      // a save never changes the baseline
});

test('a value not in the choices is kept; off before dim is refused', async ({ page, request }) => {
  await request.post('/__presence', { data: { dim_s: 10, off_s: 20 } });     // the harness's short timings
  await open(page);
  await expect(page.locator('#scr-dim option:checked')).toHaveText('10 s');
  await expect(page.locator('#scr-off option:checked')).toHaveText('30 s');
  await page.locator('#scr-dim').selectOption('3600');
  await page.locator('#scr-off').selectOption('1800');
  await page.locator('#scr-save').click();
  await expect(page.locator('#msg')).toHaveText('Turning off must come after dimming.');
  expect(await posts(request, '/api/presence')).toEqual([]);
});

test('calibrate shows "Calibrating", then the new background noise', async ({ page, request }) => {
  await open(page);
  await expect(page.locator('#scr-state')).toContainText('Active');
  await page.locator('#scr-cal').click();
  await expect(page.locator('#msg')).toHaveText('Stay quiet: measuring the background noise for 5 s.');
  await expect(page.locator('#scr-state')).toContainText('Calibrating... ');
  await expect(page.locator('#scr-cal')).toBeDisabled();
  expect((await posts(request, '/api/calibrate')).map(e => e.body)).toEqual([{ seconds: 5 }]);
  // The mock ends it after 5 polls (1 s apart) with cal "ok" and a baseline of -66 dBFS
  await expect(page.locator('#msg')).toHaveText('Background noise measured: -66 dBFS.', { timeout: 15000 });
  await expect(page.locator('#msg')).not.toHaveClass('bad');
  await expect(page.locator('#scr-state')).toContainText('Active');
  await expect(page.locator('#scr-cal')).toBeEnabled();
});

test('a calibration in a noisy room says the previous level was kept', async ({ page, request }) => {
  await open(page);
  await expect(page.locator('#scr-state')).toContainText('Active');
  await page.locator('#scr-cal').click();
  await expect(page.locator('#scr-state')).toContainText('Calibrating... ');
  await request.post('/__presence', { data: { calibrating: false, calib_left_s: 0, cal: 'noisy', cal_spread_db: 18 } });
  await expect(page.locator('#msg')).toHaveText(
    'The room wasn\'t quiet enough: the previous level (-60 dBFS) was kept. Try again in silence.', { timeout: 10000 });
  await expect(page.locator('#msg')).toHaveClass('bad');
  await expect(page.locator('#scr-cal')).toBeEnabled();
  expect((await state(request)).presence.baseline_db).toBe(-60);
});

test('an earlier calibration\'s verdict is not shown on opening the page', async ({ page, request }) => {
  await request.post('/__presence', { data: { cal: 'noisy' } });
  await open(page);
  await expect(page.locator('#scr-state')).toContainText('Active');
  await page.waitForTimeout(3500);                                      // a second poll
  await expect(page.locator('#msg')).toBeEmpty();
});

test('states: dimmed, off, no microphone', async ({ page, request }) => {
  await request.post('/__presence', { data: { state: 'dim', brightness: 15 } });
  await open(page);
  await expect(page.locator('#scr-state')).toHaveText('Dimmed · brightness 15 %');
  await request.post('/__presence', { data: { state: 'off', brightness: 0 } });
  await expect(page.locator('#scr-state')).toHaveText('Off · brightness 0 %', { timeout: 10000 });
  await request.post('/__presence', { data: { mic_ok: false, state: 'active' } });
  await expect(page.locator('#scr-state')).toHaveText('No microphone: the screen stays on', { timeout: 10000 });
  await expect(page.locator('#scr-cal')).toBeDisabled();
});

test('no microphone: calibrate is refused with a message', async ({ page, request }) => {
  await open(page);
  await expect(page.locator('#scr-cal')).toBeEnabled();
  await request.post('/__presence', { data: { mic_ok: false } });
  await page.locator('#scr-cal').click();                    // before the next poll disables it
  await expect(page.locator('#msg')).toHaveText("Can't measure now: no microphone, or a measure is already running.");
});

test('no motion sensor: no motion option', async ({ page, request }) => {
  await request.post('/__presence', { data: { imu_ok: false } });
  await open(page);
  await expect(page.locator('#scr-state')).toContainText('Active');
  await expect(page.locator('#scr-motion-box')).toBeHidden();
});

test('the screen texts in English and in French', async ({ page, request }) => {
  await open(page);
  const I18N = await table(page);
  for (const k of ['screen', 'dim_quiet', 'dim_after', 'off_after', 'bright', 'dimmed', 'motion_wake', 'save',
                   'calibrate', 'cal_hint', 'st_active', 'st_dim', 'st_off', 'st_cal', 'no_mic', 'state_line', 'level',
                   'cal_start', 'cal_na', 'cal_ok', 'cal_noisy', 'off_longer', 'not_saved', 'n_s', 'n_min', 'n_h']) {
    expect(I18N.en[k], `I18N.en.${k}`).toBeTruthy();
    expect(I18N.fr[k], `I18N.fr.${k}`).toBeTruthy();
  }
  await expect(page.locator('#screen h2')).toHaveText('Screen');
  await page.locator('#lang').selectOption('fr');
  await expect.poll(async () => (await state(request)).info.lang).toBe('fr');
  await expect(page.locator('#screen h2')).toHaveText('Écran');
  await expect(page.locator('#screen')).toContainText('Tamiser puis éteindre l\'écran quand la pièce est calme');
  await expect(page.locator('#scr-state')).toHaveText('Allumé · luminosité 100 %');
  await expect(page.locator('#scr-dim option:checked')).toHaveText('10 min');
  await expect(page.locator('#scr-off option:checked')).toHaveText('1 h');
  await page.locator('#scr-cal').click();
  await expect(page.locator('#scr-state')).toContainText('Mesure du bruit de fond...');
  await request.post('/__presence', { data: { calibrating: false, calib_left_s: 0, cal: 'noisy' } });
  await expect(page.locator('#msg')).toContainText('La pièce n\'était pas assez silencieuse', { timeout: 10000 });
});

test('saving needs the key', async ({ page, request }) => {
  await open(page, '');
  await expect(page.locator('#scr-state')).toContainText('Active');           // reading doesn't
  await page.locator('#scr-save').click();
  await expect(page.locator('#msg')).toHaveText(/scanning the QR code/);
  expect((await posts(request, '/api/presence')).map(e => e.refused)).toEqual([401]);    // refused, nothing saved
  expect((await state(request)).presence.dim_s).toBe(600);
});

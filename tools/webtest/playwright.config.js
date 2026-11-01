// Browser tests of the settings page (main/web/index.html) against a mock display (mock-server.js).
//   cd tools/webtest && npm test            (first time: npm install && npx playwright install chromium)
// Screenshots of each test land in tools/webtest/shots/ for review.
const { defineConfig, devices } = require('@playwright/test');

module.exports = defineConfig({
  testDir: './tests',
  // One mock display per worker, on its own port (fixtures.js starts it): tests in different workers never share
  // its state. 1 worker took ~45 s for the 33 tests, 4 take ~15 s.
  workers: process.env.WEBTEST_WORKERS ? Number(process.env.WEBTEST_WORKERS) : 4,
  fullyParallel: true,
  reporter: [['list']],
  use: {
    ...devices['Pixel 7'],                       // the page is used on a phone
    browserName: 'chromium',
  },
});

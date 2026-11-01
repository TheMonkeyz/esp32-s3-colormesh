// Loaded first in the display's settings page under the emulator (build/settings.html, made by the Makefile from
// main/web/index.html): its requests to /api/ go to the emulated display in the page around it (index.html,
// emuApi(), served by emu_web.c) instead of the network, and it tells that page its height (the iframe follows it).
// The frame can start before the page around it has run its script: a request waits for emuApi to exist.
(() => {
  const real = window.fetch.bind(window);
  const framed = window.parent !== window;
  const emuApi = () => new Promise(ready => {
    const look = () => (window.parent.emuApi ? ready(window.parent.emuApi) : setTimeout(look, 50));
    look();
  });
  window.fetch = async (url, opt = {}) => {
    const u = typeof url === 'string' ? url : url.url;
    if (!framed || !u.startsWith('/api/')) return real(url, opt);
    const r = await (await emuApi())(opt.method || 'GET', u, typeof opt.body === 'string' ? opt.body : '');
    return new Response(r.body, { status: r.status, headers: { 'Content-Type': 'application/json' } });
  };
  const report = () => window.parent.postMessage({ emuSettingsHeight: document.documentElement.scrollHeight }, location.origin);
  addEventListener('load', () => { report(); new ResizeObserver(report).observe(document.body); });
})();

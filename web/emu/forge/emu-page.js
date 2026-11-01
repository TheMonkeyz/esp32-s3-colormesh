// The display in the page (the framework's part; the app's index.html has the words and the look). Loaded after
// emu.js, it needs from the page: <canvas id="screen" width="DISP_W" height="DISP_H">, an element id="status"
// (its data-error is the text shown if the emulator can't start, followed by the reason), and optionally
// <iframe id="settings" src="settings.html#k=0000000000000000"> for the settings page.
//   - draws the firmware's framebuffer (RGB565, emu_display.c) on the canvas when it changed, dimmed as
//     display_brightness() asks (a CSS filter; 0 is black, as an AMOLED off);
//   - the mouse or a finger on the canvas is the touch (emu_touch.c);
//   - the settings page's /api/ requests (emu-settings.js in the frame) are queued here and served by the firmware's
//     own handlers between frames (emu_web.c reads Module.emuApiQ, answers through Module.emuApiWait); the frame says
//     its height and follows it.
// window.emuReady resolves to the Emscripten module, for an app's own controls (weather_amoled: microphone, motion).
(() => {
  const apiQ = [], apiWait = {};
  let apiId = 0;
  window.emuApi = (method, path, body) => new Promise(done => {
    const id = ++apiId;
    apiWait[id] = done;
    apiQ.push({ id, method, path, body });
  });
  addEventListener('message', e => {
    const frame = document.getElementById('settings');
    if (frame && e.origin === location.origin && e.data && e.data.emuSettingsHeight)
      frame.style.height = Math.max(400, e.data.emuSettingsHeight) + 'px';
  });
  const status = document.getElementById('status');
  window.emuReady = (async () => {
    const canvas = document.getElementById('screen'), ctx = canvas.getContext('2d');
    const W = canvas.width, H = canvas.height;
    const img = ctx.createImageData(W, H), rgba = new Uint32Array(img.data.buffer);
    const M = await ForgeEmu({ print: t => console.log(t), printErr: t => console.log(t) });
    M.emuApiQ = apiQ;
    M.emuApiWait = apiWait;
    if (status) status.textContent = '';
    const fb = M._emu_fb() >> 1;                     // the RGB565 framebuffer, in 16-bit words
    let shown = 255;
    // RGB565 -> RGBA (little-endian Uint32: A B G R), whenever the firmware drew something
    const draw = () => {
      if (M._emu_fb_dirty()) {
        const p = M.HEAPU16.subarray(fb, fb + W * H);
        for (let i = 0; i < p.length; i++) {
          const v = p[i];
          const r = (v >> 11) * 255 / 31, g = ((v >> 5) & 63) * 255 / 63, b = (v & 31) * 255 / 31;
          rgba[i] = 0xff000000 | (b << 16) | (g << 8) | r;
        }
        ctx.putImageData(img, 0, 0);
      }
      const level = M._emu_brightness();
      if (level !== shown) {
        shown = level;
        canvas.style.filter = level >= 255 ? '' : `brightness(${(level / 255).toFixed(3)})`;
      }
      requestAnimationFrame(draw);
    };
    requestAnimationFrame(draw);
    // The finger: pointer events in the canvas's own pixels
    const at = e => {
      const r = canvas.getBoundingClientRect();
      return [Math.round((e.clientX - r.left) * W / r.width), Math.round((e.clientY - r.top) * H / r.height)];
    };
    let down = false;
    canvas.addEventListener('pointerdown', e => { down = true; canvas.setPointerCapture(e.pointerId); M._emu_touch(1, ...at(e)); });
    canvas.addEventListener('pointermove', e => { if (down) M._emu_touch(1, ...at(e)); });
    const up = e => { if (!down) return; down = false; M._emu_touch(0, ...at(e)); };
    canvas.addEventListener('pointerup', up);
    canvas.addEventListener('pointercancel', up);
    canvas.addEventListener('contextmenu', e => e.preventDefault());   // (a long press on a phone)
    return M;
  })();
  window.emuReady.catch(e => {
    if (status) status.textContent = (status.dataset.error || 'The emulator could not start:') + ' ' + e;
    console.error(e);
  });
})();

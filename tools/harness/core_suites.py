"""Generic test suites: valid for any espforge firmware (forge_core, forge_net, forge_lvgl, forge_ota) whatever the
app. Each test is a function(ctx) that raises board.Fail on a failed check; ctx.metric() records numbers for the
baseline, ctx.note() adds a line to the report, ctx.skip() says why a metric wasn't measured. Contract:
docs/PROTOCOL.md; project values: forge.json (board.CFG). The app's own tests are in app_suites.py.
"""
import os
import re
import shutil
import subprocess
import time
import urllib.request

from board import CFG, ROOT, SETUP_IP, Fail, check, dns_query, http_get, kv, test
import forgecfg
import snapshot

# Suites that only run when asked for (or with their flag): ota needs --ota
ON_REQUEST = {'ota'}
# Left out by --quick (iterating on a change): an idle minute, the PC joining the setup network, and the Playwright
# suite (board-free: npm test and CI run it). The full run, before a release candidate, has them.
QUICK_SKIP = {'idle_stable', 'wifi_setup'}
QUICK = False


def ms_of(line):
    m = re.match(r'[IWE] \((\d+)\)', line)
    return int(m.group(1)) if m else None


def last_boot(lines):
    """The lines of the last start-up in the window ([] when the window began after it: a reused log window)."""
    starts = [i for i, l in enumerate(lines) if 'rst:0x' in l or 'ESP-ROM:' in l]
    if starts:
        return lines[starts[-1]:]
    return lines if any(re.search(CFG['ready_line'], l) for l in lines) else []


# ---------------------------------------------------------------- boot

@test('boot')
def start_up(ctx):
    """The last start-up in the log: the app version ESP-IDF prints, the ready line, no error lines, the OTA line."""
    lines = last_boot(ctx.log.lines())
    if not lines:
        ctx.skip('boot_s.*', 'no start-up in this log window (it was reused): restart the board to measure it')
        ctx.note('boot: no start-up in this log window, nothing checked')
        return
    v = ctx.board.version()
    # ESP-IDF's own boot lines (App version...) come in the first ~2.5 s, while the USB Serial/JTAG port is still
    # re-enumerating after the reset: the monitor usually misses them. Checked when present; the OTA line below is
    # the one that counts.
    m = next((re.search(r'App version:\s+(\S+)', l) for l in lines if 'App version:' in l), None)
    if m:
        check(m.group(1) == v, f'ESP-IDF says App version {m.group(1)}, the console says {v}')
    ready = next((l for l in lines if re.search(CFG['ready_line'], l)), None)
    check(ready, f'no ready line /{CFG["ready_line"]}/ in the start-up')
    if ms_of(ready) is not None:
        ctx.metric('boot_s.ready', round(ms_of(ready) / 1000, 1))
    for l in lines:                                    # the stages, for the report
        mk = re.search(r'diag: mark (.+?)(?:\s+internal (\d+) KB|$)', l)
        if mk and ms_of(l) is not None:
            ctx.note(f'boot stage "{mk.group(1).strip()}" at {ms_of(l) / 1000:.1f} s'
                     + (f', internal {mk.group(2)} KB free' if mk.group(2) else ''))
    errors = [l for l in lines if l.startswith('E (')]
    check(not errors, f'{len(errors)} error lines in the start-up, first: {errors[0][:120]}' if errors else '')
    ota = next((re.search(r'ota: Running (\S+) from (\w+), channel (\w+)', l) for l in lines if 'ota: Running' in l), None)
    check(ota, 'no "ota: Running <ver> from <slot>, channel <ch>" line in the start-up')
    check(ota.group(1) == v, f'the OTA line says {ota.group(1)}, the console says {v}')
    reset = next((l.split('diag: ', 1)[1] for l in lines if 'diag: boot ' in l), '')
    ctx.note(f'{v} from {ota.group(2)}, channel {ota.group(3)}' + (f'; {reset}' if reset else ''))
    crash = next((l.split('diag: ', 1)[1] for l in lines if 'diag: coredump: last crash' in l), '')
    if crash:
        ctx.note(f'the board kept a crash from before this start-up: {crash[:140]}')


# ---------------------------------------------------------------- console

@test('console')
def commands(ctx):
    """ping, help, heap and where answer in the expected format (PROTOCOL.md §2)."""
    b = ctx.board
    v = b.version()
    names = b.cmd('help', r'test: commands: (.*)').group(1)
    have = {c.strip().split(' ')[0] for c in names.split(',')}
    for want in ('ping', 'help', 'heap', 'where', 'reboot', 'key', 'wifi'):    # forge_core and forge_net
        check(want in have, f'"help" does not list "{want}" ({names[:200]})')
    ui = [c for c in ('screen', 'tap', 'press', 'swipe', 'drag', 'fps') if c not in have]
    if ui:
        ctx.note('no display commands: ' + ', '.join(ui) + ' (a firmware without forge_lvgl?)')
    h = b.heap()
    for k in ('internal', 'min', 'largest', 'psram', 'psram_min', 'uptime_s', 'failed_allocs', 'lvgl_fallbacks'):
        check(k in h, f'"heap" lacks {k}= ({h})')
    w = b.cmd('where', r'test: where (.*)').group(1)
    check('task_lvgl=' in w, f'"where" lacks task_lvgl= ({w})')
    try:
        b.cmd('no-such-command', timeout=5)
        refused = 'answered'
    except Fail as e:
        refused = str(e)
    check('unknown command' in refused, f'an unknown command was not refused as unknown ({refused[:120]})')
    ctx.note(f'{v}: {len(have)} console commands; where: {w[:100]}')


@test('console')
def shot_over_usb(ctx):
    """`shot SCREEN [DIV]` (v0.6.0): a screen rendered off-display and sent over the USB console as base64 RGB565 rows,
    for a session that can't reach the board's HTTPS (tools/snapshot.py) and for screens before Wi-Fi;
    tools/devloop/shot.py turns a log into PNGs. Fails on the old firmware: no such command."""
    import shot
    b = ctx.board
    at = len(ctx.log.lines())
    name = (CFG['screens'] or ['current'])[0]
    m = b.cmd(f'shot {name} 4', r'test: ok shot (\S+) (\d+)x(\d+)', timeout=30)
    ctx.log.wait(r'^SHOTEND', 5, 'the end of the shot', start=at)
    got = list(shot.blocks(ctx.log.lines()[at:]))
    check(got and got[-1][0] == name, f'no SHOT block for {name} decoded from the log ({len(got)} blocks)')
    _, w, h, rgb, rows = got[-1]
    check((w, h) == (int(m.group(2)), int(m.group(3))) and w == CFG['screen']['w'] // 4,
          f'{w}x{h} decoded, the console said {m.group(2)}x{m.group(3)}, a quarter of the screen expected')
    lit = sum(1 for i in range(0, len(rgb), 3) if rgb[i] + rgb[i + 1] + rgb[i + 2] > 60)
    check(rows == h and lit > w * h // 100, f'{rows} of {h} rows, {lit} lit pixels of {w * h}: a broken or blank picture')  # hello: text on black, ~4 %
    shot.png(ctx.out(f'shot_{name}.png'), w, h, rgb)
    ctx.note(f'shot {name} 4: {w}x{h} over USB, {rows} rows, {lit} lit pixels (shot_{name}.png)')


@test('console')
def wifi_low_latency(ctx):
    """net_low_latency() (v0.6.0): Wi-Fi power save off while a screen wants fast requests (ESP-IDF's default costs
    ~100 ms a request, MeteoBus's radar), counted; `wifi status` shows ps, lowlat, the download slot and queued jobs."""
    b = ctx.board
    w = b.wifi()
    check('ps' in w and 'lowlat' in w and 'dl' in w and 'bg' in w, f'wifi status lacks ps/lowlat/dl/bg: {w}')
    check(w['ps'] == 'min' and w['lowlat'] == '0', f'at rest: ps={w["ps"]} lowlat={w["lowlat"]}, expected min / 0')
    try:
        w = kv(b.cmd('wifi lowlat on', r'test: wifi (.*)').group(1))
        check(w['ps'] == 'none' and w['lowlat'] == '1', f'lowlat on: ps={w["ps"]} lowlat={w["lowlat"]}, expected none / 1')
    finally:
        w = kv(b.cmd('wifi lowlat off', r'test: wifi (.*)').group(1))
    check(w['ps'] == 'min' and w['lowlat'] == '0', f'lowlat off: ps={w["ps"]} lowlat={w["lowlat"]}, expected min / 0')
    ctx.note(f'wifi lowlat on|off: power save none|min; download slot {w["dl"]}, background jobs {w["bg"]}')


# ---------------------------------------------------------------- memory

def heap_settled(read, settle, tries=15, step=2.0, within_kb=2):
    """The heap numbers once they stop moving. Right after an OTA install the update's TLS connection may still hold
    its buffers (~10 KB internal): an --ota run read internal_largest_kb 35 there and 46 at rest on the same firmware
    (2026-10-08). With `settle`, read until two readings are within `within_kb` on internal and largest (`tries` at most)."""
    h = read()
    if not settle:
        return h
    for _ in range(tries):
        time.sleep(step)
        n = read()
        if abs(n['internal'] - h['internal']) <= within_kb and abs(n['largest'] - h['largest']) <= within_kb:
            return n
        h = n
    return h


@test('memory')
def heap_numbers(ctx):
    h = heap_settled(ctx.board.heap, settle=bool(getattr(ctx.opts, 'ota', None)))   # an --ota run: after the install
    ctx.metric('internal_free_kb', h['internal'])
    ctx.metric('internal_min_kb', h['min'])
    ctx.metric('internal_largest_kb', h['largest'])
    ctx.metric('psram_min_kb', h['psram_min'])
    ctx.metric('failed_allocs', h['failed_allocs'])          # any allocation that failed since boot
    ctx.metric('lvgl_internal_fallbacks', h['lvgl_fallbacks'])   # LVGL blocks in internal RAM (PSRAM full)


# ---------------------------------------------------------------- idle

@test('idle_stable')
def sixty_seconds_idle(ctx):
    """A minute of nothing: no restart (the harness checks), no crash line, internal RAM not falling."""
    b = ctx.board
    h0 = b.heap()
    at = len(ctx.log.lines())
    time.sleep(60)
    h1 = b.heap()
    crash = [l for l in ctx.log.lines()[at:] if re.search(r'Guru Meditation|abort\(\)|Backtrace:', l)]
    check(not crash, f'crash while idle: {crash[0][:120]}' if crash else '')
    check(h1['uptime_s'] >= h0['uptime_s'] + 55, f'uptime went from {h0["uptime_s"]} to {h1["uptime_s"]} s (a restart?)')
    drop = h0['internal'] - h1['internal']
    ctx.metric('idle_internal_drop_kb', max(0, drop))
    ctx.note(f'idle 60 s: internal {h0["internal"]} -> {h1["internal"]} KB, PSRAM {h0["psram"]} -> {h1["psram"]} KB')


# ---------------------------------------------------------------- screens

@test('screens')
def every_screen(ctx):
    """Each screen in forge.json "screens": shown through the console, snapshot saved, not blank. Those in
    "screens_not_shown" are only snapshotted (the firmware prepares their texts off-display): showing them would change
    the device's state (the starter's Easy Connect page takes the radio off the home network)."""
    b = ctx.board
    names = CFG['screens']
    hidden = set(CFG.get('screens_not_shown') or [])
    if not names:
        ctx.note('forge.json lists no screens: nothing checked')
        return
    # The shown ones first, then back home, then the ones only snapshotted: their texts are prepared off-display only
    # while no other state of theirs is open (the starter's setup page 1 while setup is open on page 0)
    order = [n for n in names if n not in hidden] + [n for n in names if n in hidden]
    for i, name in enumerate(order):
        if name in hidden and (i == 0 or order[i - 1] not in hidden):
            b.show(names[0])
            b.wait_screen(names[0], 6)
        if name not in hidden:
            b.show(name)
            b.wait_screen(name, 6)
            time.sleep(0.5)
        ms, bmp = b.snap(name, ctx.out(f'screen_{name}.png'))
        ctx.snapped.add(name)
        ctx.metric(f'snapshot_ms.{name}', round(ms))
        w, h, _ = snapshot.bmp_pixels(bmp)
        check((w, h) == (CFG['screen']['w'], CFG['screen']['h']),
              f'snapshot of {name} is {w}x{h}, forge.json says {CFG["screen"]["w"]}x{CFG["screen"]["h"]}')
        check(snapshot.distinct_colors(bmp) > 1, f'the snapshot of {name} is blank (one colour)')
    b.show(names[0])
    b.wait_screen(names[0], 6)
    ctx.note('screens: ' + ', '.join(names))


# ---------------------------------------------------------------- web

_webtest = {}


def webtest_start():
    """Start the Playwright suite in the background when the run begins: it needs no board, so it runs while the
    board's suites do (~15-50 s saved); settings_page_tests collects it."""
    wt = os.path.join(ROOT, 'tools', 'webtest')
    if QUICK or _webtest or not os.path.isdir(os.path.join(wt, 'node_modules')):
        return
    env = dict(os.environ)
    # Microsoft Store Python virtualises AppData\Local for its child processes, so Playwright can't see the browsers
    # in AppData\Local\ms-playwright. Use a copy next to the tests (git-ignored).
    browsers = os.path.join(wt, '.browsers')
    if os.path.isdir(browsers):
        env['PLAYWRIGHT_BROWSERS_PATH'] = browsers
    npm = shutil.which('npm') or r'C:\Program Files\nodejs\npm.cmd'
    if not os.path.exists(npm):
        return                                     # settings_page_tests says so
    import tempfile
    out = tempfile.TemporaryFile(mode='w+', encoding='utf-8', errors='replace')
    _webtest.update(proc=subprocess.Popen([npm, 'test'], cwd=wt, env=env, stdout=out, stderr=subprocess.STDOUT,
                                          text=True), out=out)


@test('web')
def settings_page_tests(ctx):
    """Playwright suite against the mock display (tools/webtest), when it is installed."""
    wt = os.path.join(ROOT, 'tools', 'webtest')
    if QUICK:
        ctx.note('Playwright: left out by --quick (cd tools/webtest && npm test)')
        return
    if not os.path.isdir(os.path.join(wt, 'node_modules')):
        ctx.note('tools/webtest not installed (npm install && npx playwright install chromium): page tests not run')
        return
    if _webtest:                                    # started with the run (webtest_start)
        try:
            rc = _webtest['proc'].wait(timeout=600)
        except subprocess.TimeoutExpired:
            _webtest['proc'].kill()
            raise Fail('Playwright: no result within 10 min')
        _webtest['out'].seek(0)
        out = _webtest['out'].read()
        m = re.search(r'(\d+) passed', out)
        failed = re.search(r'(\d+) failed', out)
        open(ctx.out('webtest.txt'), 'w', encoding='utf-8').write(out)
        check(rc == 0 and m and not failed, f'Playwright: {failed.group(0) if failed else "error"} (webtest.txt)')
        ctx.note(f'Playwright: {m.group(0)}')
        return
    env = dict(os.environ)
    # Microsoft Store Python virtualises AppData\Local for its child processes, so Playwright can't see the browsers
    # in AppData\Local\ms-playwright. Use a copy next to the tests (git-ignored).
    browsers = os.path.join(wt, '.browsers')
    if os.path.isdir(browsers):
        env['PLAYWRIGHT_BROWSERS_PATH'] = browsers
    npm = shutil.which('npm') or r'C:\Program Files\nodejs\npm.cmd'
    if not os.path.exists(npm):
        raise Fail('Node.js (npm) not found (macOS: brew install node, then cd tools/webtest && npm ci)')
    r = subprocess.run([npm, 'test'], cwd=wt, env=env, capture_output=True, text=True, errors='replace', timeout=600)
    out = r.stdout + r.stderr
    m = re.search(r'(\d+) passed', out)
    failed = re.search(r'(\d+) failed', out)
    open(ctx.out('webtest.txt'), 'w', encoding='utf-8').write(out)
    check(r.returncode == 0 and m and not failed, f'Playwright: {failed.group(0) if failed else "error"} (webtest.txt)')
    ctx.note(f'Playwright: {m.group(0)}')


@test('web')
def live_api(ctx):
    """The page arrives whole and /api/info has every field (PROTOCOL.md §4)."""
    b = ctx.board
    with urllib.request.urlopen(forgecfg.https_base(CFG, b.ip) + '/', context=b.ctx, timeout=15) as r:
        page = r.read()
    check(b'</html>' in page[-200:], 'settings page arrived truncated (the hardware-AES / DMA memory bug class)')
    ctx.metric('page_kb', len(page) // 1024)
    info = b.api('/api/info')
    for k in ('app', 'version', 'ip', 'ssid', 'rssi', 'uptime_s', 'setup', 'lang', 'languages'):
        check(k in info, f'GET /api/info: no "{k}" ({sorted(info)})')
    check(info['app'] == CFG['app'], f'/api/info app is "{info["app"]}", forge.json says "{CFG["app"]}"')
    check(info['version'] == ctx.version, f'/api/info version {info["version"]}, the console says {ctx.version}')
    check(info['ip'] == b.ip, f'/api/info ip {info["ip"]}, the harness talks to {b.ip}')
    check(info['setup'] is False, 'GET /api/info says setup=true on the home network')
    codes = [l.get('code') for l in info['languages']]
    check(info['lang'] in codes, f'lang "{info["lang"]}" is not in languages {codes}')
    u = b.api('/api/update')
    for k in ('current', 'latest', 'channel', 'state', 'progress', 'error', 'pending_verify', 'uptime_s'):
        check(k in u, f'GET /api/update: no "{k}" ({sorted(u)})')
    ctx.note(f'/api/info: {info["app"]} {info["version"]} on "{info["ssid"]}" ({info["rssi"]} dBm), lang {info["lang"]}')


@test('web')
def who_may_change(ctx):
    """forge_net web: the key on every POST and on snapshots, JSON bodies, the device's own Host."""
    b = ctx.board
    if not b.key():
        raise Fail('the test console gave no key ("key" command)')
    js = {'Content-Type': 'application/json'}
    k = {'X-Key': b.key()}
    lang = b.api('/api/info')['lang']
    body = '{"lang":"%s"}' % lang                     # the current language again: changes nothing
    st, _ = b.request('POST', '/api/settings', body, js)
    check(st == 401, f'POST without the key: {st}, expected 401')
    st, _ = b.request('POST', '/api/settings', body, {**js, 'X-Key': '0' * 16})
    check(st == 401, f'POST with a wrong key: {st}, expected 401')
    st, _ = b.request('POST', '/api/settings', body, {'Content-Type': 'text/plain', **k})
    check(st == 415, f'POST as text/plain: {st}, expected 415')
    st, _ = b.request('GET', '/api/info', host='rebind.example')
    check(st == 421, f'GET with another Host (DNS rebinding): {st}, expected 421')
    st, _ = b.request('GET', '/api/snapshot?screen=' + (CFG['screens'] or ['current'])[0])
    check(st == 401, f'snapshot without the key: {st}, expected 401')
    st, loc = b.request('GET', '/', tls=False, host='evil.example')
    home = forgecfg.https_base(CFG, b.ip) + '/'                  # with the port when it isn't 443 (web_port)
    check(st in (301, 302, 307, 308) and loc == home,
          f'plain HTTP on the home network: {st} {loc}, expected a redirect to {home} (not the Host header)')
    st, _ = b.request('POST', '/api/settings', body, {**js, **k})
    check(st == 200, f'POST with the key: {st}, expected 200')
    ctx.note('settings API: 401 without/with a wrong key, 415, 421, snapshot 401, HTTP redirect, 200 with the key')


# ---------------------------------------------------------------- update

NETWORK_WORDS = ('network', 'connect', 'timeout', 'timed out', 'dns', 'http', 'tls', 'resolve', 'unreachable')


@test('update')
def check_for_updates(ctx):
    """POST check: the state moves on to up_to_date or available, never failed (except the network)."""
    b = ctx.board
    u = b.api('/api/update', {'action': 'check'})
    end = time.time() + 60
    while u.get('state') in ('checking', 'idle') and time.time() < end:
        time.sleep(1)
        u = b.api('/api/update')
    if u.get('state') == 'failed' and any(w in (u.get('error') or '').lower() for w in NETWORK_WORDS):
        ctx.note(f'update check failed on the network ({u.get("error")}): not counted against the firmware')
        return
    check(u.get('state') in ('up_to_date', 'available'),
          f'update check ended in state "{u.get("state")}" (error "{u.get("error")}")')
    check(u.get('current') == ctx.version, f'/api/update current {u.get("current")}, the console says {ctx.version}')
    ctx.note(f'update check: {u["state"]} on {u.get("channel")} (latest {u.get("latest") or "-"})')


# ---------------------------------------------------------------- Wi-Fi: lost at run time

@test('wifi_runtime')
def lose_and_recover(ctx):
    """The saved network disappears while running ("wifi offline"): retries go on; it comes back ("wifi online"):
    connected again within 60 s."""
    b = ctx.board
    b.cmd('wifi offline', r'test: wifi (.*)')
    end = time.time() + 15
    while b.wifi()['connected'] == '1':
        if time.time() > end:
            raise Fail('still connected 15 s after the network was taken away')
        time.sleep(1)
    r0 = int(b.wifi()['retries'])
    time.sleep(8)
    w = b.wifi()
    if w.get('ap') == '1':
        ctx.note('the setup network opened while offline (retries paused)')
    else:
        check(int(w['retries']) > r0, f'no reconnect attempts while offline (retries {r0} -> {w["retries"]})')
    t0 = time.time()
    b.cmd('wifi online', r'test: wifi (.*)')
    end = t0 + 60
    while b.wifi()['connected'] != '1':
        if time.time() > end:
            raise Fail('did not reconnect within 60 s after the network came back')
        time.sleep(1)
    ctx.metric('reconnect_s', round(time.time() - t0, 1))
    ctx.note(f'offline: retries {r0} -> {w["retries"]}; reconnected {time.time() - t0:.0f} s after "wifi online"')


# ---------------------------------------------------------------- Wi-Fi: setup while offline

def back_online(ctx, since, timeout=90):
    """'wifi online', close setup if it is shown (a tap), wait for the station's IP line."""
    b = ctx.board
    b.cmd('wifi online', r'test: wifi (.*)')
    if 'setup' in CFG['screens'] and b.screen() == 'setup':
        b.tap()
    ctx.log.wait(CFG['ip_line'], timeout, 'connected again after the setup test', start=since)


@test('wifi_setup')
def setup_network_and_portal(ctx):
    """Restart with the saved network unreachable ("wifi offline-boot-short"): the setup network opens, the PC joins
    it like a phone, DNS answers every name with the display (captive portal), the portal page is served. --phone:
    Easy Connect, a person scans the QR code."""
    b, w = ctx.board, ctx.wifi
    if not w.available():
        raise Fail('no Wi-Fi card on the PC')
    ctx.reset_ok = True                                # this test restarts the board on purpose
    at = len(ctx.log.lines())
    b.cmd('wifi offline-boot-short', r'test: ok')
    ctx.log.wait(r'test: console ready|ota: Running', 40, 'the restart', start=at)   # (L154: the first may be missed)
    end = time.time() + 75
    while b.wifi().get('ap') != '1':
        if time.time() > end:
            raise Fail('the setup network did not open within 75 s of an offline start-up')
        time.sleep(2)
    st = b.wifi()
    check(st['connected'] == '0', f'connected on an offline start-up? {st}')
    b.cmd('portal windows-quiet')                     # no browser tab popping up on this PC
    ssid = w.find_setup()
    ctx.note(f'setup network "{ssid}" seen by the PC' if ssid else
             f'"{CFG["setup_ssid"]}" not in the scan Windows last made: joining it by name')
    ssid = ssid or CFG['setup_ssid']
    if ctx.opts.phone:
        ctx.ask(f'Easy Connect: open the setup screen\'s QR code with your Android phone\'s camera (the phone on your '
                f'home Wi-Fi) now. The harness waits 3 minutes.')
        since = len(ctx.log.lines())
        ctx.log.wait(CFG['ip_line'], 180 + 90, 'the phone\'s network received, the display connected', start=since)
        ctx.note('Easy Connect: the phone sent the network, the display connected')
        return                                         # its restart cleared the fake network
    w.join_setup(st.get('ap_pass', ''), ssid)
    try:
        for name in ('connectivitycheck.gstatic.com', 'captive.apple.com', 'example.org'):
            a = dns_query(SETUP_IP, name)
            check(a == SETUP_IP, f'DNS for {name} gave {a}, expected {SETUP_IP} (the captive portal needs it)')
        code, hdr, _ = http_get(SETUP_IP, '/generate_204', 'connectivitycheck.gstatic.com')
        check(code == 302 and hdr.get('Location', '').startswith('http://' + SETUP_IP),
              f'Android connectivity check: HTTP {code} {hdr.get("Location")}, expected 302 to the portal')
        code, _, body = http_get(SETUP_IP, '/', SETUP_IP)
        check(code == 200 and b'</html>' in body[-300:], f'portal page: HTTP {code}, {len(body)} bytes')
        code, _, body = http_get(SETUP_IP, '/api/info', SETUP_IP)
        check(code == 200 and b'"setup":true' in body.replace(b' ', b''), f'GET /api/info on the setup network: HTTP {code}')
        nows = body.replace(b' ', b'')
        check(b'"ssid":""' in nows and b'"ip":""' in nows,
              'the setup network gets the home network name or address from /api/info')
        code, _, _ = http_get(SETUP_IP, '/api/snapshot', SETUP_IP)
        check(code in (401, 403), f'snapshot on the setup network: HTTP {code}, expected 401/403')
        check(b.wifi().get('ap_clients', '0') != '0',
              'the display does not count the PC as a setup client')
        n0 = int(b.wifi()['retries'])
        time.sleep(10)                                 # a reconnect attempt used to knock phones off here
        state, now = w.state()
        check(state == 'connected' and now == ssid, f'the PC dropped off the setup network ({state} {now})')
        check(int(b.wifi()['retries']) == n0, 'reconnect attempts while a phone is on the setup network')
    finally:
        w.leave()
    since = len(ctx.log.lines())
    back_online(ctx, since)
    ctx.note('offline start-up: setup network, PC joined, captive DNS, portal page, back online')


# ---------------------------------------------------------------- OTA (--ota)

@test('ota')
def installed_release(ctx):
    """After --ota installed the release: it runs, is confirmed, and says so."""
    want = ctx.opts.ota
    if not want:
        raise Fail('the ota suite needs --ota VERSION')
    u = ctx.board.api('/api/update')
    check(u.get('current') == want, f'/api/update current {u.get("current")}, expected {want}')
    check(not u.get('pending_verify'), f'{want} is not confirmed yet (pending_verify)')
    check(not u.get('rolled_back'), 'the updater reports a rollback')
    lines = ctx.log.lines()
    m = next((re.search(r'ota: Running (\S+) from (\w+)', l) for l in reversed(lines) if 'ota: Running' in l), None)
    check(m and m.group(1) == want, f'the last "ota: Running" line says {m.group(1) if m else "nothing"}, not {want}')
    ctx.note(f'{want} installed by the display\'s updater, running from {m.group(2)}, confirmed')

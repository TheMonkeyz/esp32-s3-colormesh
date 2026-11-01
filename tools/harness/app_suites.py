"""The starter app's own tests (main/: the hello and system screens, setup on a long-press). Replace or extend them
with your app's: same registry (@test from board.py), same ctx as core_suites.py.

    APP_ORDER       the app's suites, run after the core start-up checks (harness.py)
    APP_WATCH       log lines that aren't failures but must not go unseen: (regex, what), counted per test
    METRIC_SUITES   which suite produces a metric (prefix -> suite): a baseline metric is only expected when its
                    suite ran
"""
import re
import time

from board import CFG, SCREEN_C, check, test

APP_ORDER = ['navigation', 'perf', 'presence']
APP_WATCH = [
    (r'display: .*did not finish', 'a panel transfer timed out'),
    (r'lvgl: .*(out of memory|alloc failed)', 'LVGL could not allocate'),
]
METRIC_SUITES = [('drag_first_frame_ms', 'perf'), ('swipe_', 'perf'), ('setup_page_', 'navigation'), ('easy_connect_', 'navigation')]

HOME, NEXT = 'hello', 'system'                         # the starter's two pages, side by side: hello | system


def go_home(ctx):
    ctx.board.show(HOME)
    ctx.board.wait_screen(HOME, 6)
    time.sleep(0.5)


def pages(ctx):
    """The pager's screens, left to right. The starter's are fixed; an app whose page count varies (pager_set_count)
    reads it from the device here, e.g. esp32-s3-rtcquebec: one page per favourite stop from /api/favs."""
    return [HOME, NEXT]


# ---------------------------------------------------------------- navigation

@test('navigation')
def swipe_between_pages(ctx):
    """Swipe like a person: left through every page, right back; no wrap-around at either end."""
    b = ctx.board
    go_home(ctx)
    p = pages(ctx)
    route = ([('swipe left', x) for x in p[1:]] + [('swipe left', p[-1])] +      # the right end: bounces back
             [('swipe right', x) for x in p[-2::-1]] + [('swipe right', p[0])])  # the left end: bounces back
    for cmd, want in route:
        b.cmd(cmd)
        time.sleep(0.8)
        b.wait_screen(want, 6)
    ctx.note(f'{" <-> ".join(p)} by swipes, both ends bounce')


@test('navigation')
def quick_swipes(ctx):
    """A swipe that lands while the previous move's release animation still runs is a swipe too. LVGL reads nothing
    during a move, so slide.c's read hook never saw the first press end, and took the second for it: nothing moved
    (LESSONS L181). 'swipe left right' leaves 150 ms of "up" between the two
    (70 ms was sometimes read as one of the chip's brief false "ups": one drag, left then right)."""
    b = ctx.board
    go_home(ctx)
    at = len(ctx.log.lines())
    b.cmd('swipe left right')
    time.sleep(1.0)
    drags = [l for l in ctx.log.lines()[at:] if 'slide: drag: first frame' in l]
    check(len(drags) == 2, f'two quick swipes made {len(drags)} drag(s); the second was taken for the first')
    # Where the second one ends is timing: it starts when the first's release animation ends, by then the simulated
    # finger has mostly moved on ("samples 1", "back" once on v0.1.1-rc.1, on to hello in the run before)
    second = 'back' if ' back |' in drags[1] else 'on'
    ctx.note(f'two swipes 150 ms apart: 2 drags, the second went {second}; now on {b.screen()}')
    go_home(ctx)


@test('navigation')
def long_press_opens_settings(ctx):
    """A long press opens Settings (forge_settings) while online; Done goes back, and so does a swipe right."""
    b = ctx.board
    if 'settings' not in CFG['screens']:
        ctx.note('no "settings" screen in forge.json: not checked')
        return
    go_home(ctx)
    for close, how in ((lambda: b.tap(SCREEN_C[0], 42), 'Done'), (lambda: b.cmd('swipe right'), 'a swipe right')):
        b.press()
        b.wait_screen('settings', 6)
        time.sleep(1)                                  # its slide up, and the long-press's own release
        close()
        b.wait_screen(HOME, 6)
        time.sleep(0.5)
    ctx.note(f'long-press at {SCREEN_C}: Settings; Done and a swipe right: back to {HOME}')


@test('navigation')
def settings_row_acts(ctx):
    """A Settings row acts through the same setting as the phone's page: a tap on "Dim when quiet" (the first row,
    under the SCREEN title: list at y 70, title 22 px, 6 px gap, a 52 px row) switches dimming, and the page sees it."""
    b = ctx.board
    if 'settings' not in CFG['screens']:
        ctx.note('no "settings" screen in forge.json: not checked')
        return
    saved = b.api('/api/presence')
    try:
        go_home(ctx)
        b.press()
        b.wait_screen('settings', 6)
        time.sleep(1)
        at = len(ctx.log.lines())
        b.tap(SCREEN_C[0], 70 + 22 + 6 + 26)
        ctx.log.wait(r'settings: row 1\b', 4, 'the "Dim when quiet" row tapped', start=at)
        time.sleep(0.5)
        check(b.api('/api/presence')['enabled'] != saved['enabled'], 'dimming unchanged after a tap on its row')
        b.tap(SCREEN_C[0], 42)
        b.wait_screen(HOME, 6)
    finally:
        keys = ('enabled', 'margin_db', 'wake_s', 'dim_s', 'off_s', 'bright_pct', 'dim_pct', 'motion_wake')
        b.api('/api/presence', {k: saved[k] for k in keys if k in saved})
    ctx.note('a tap on "Dim when quiet" switched dimming (the page saw it); put back')


@test('navigation')
def setup_pages_slide(ctx):
    """Setup's two pages (setup network | Easy Connect) follow the finger like hello | system, and the Easy Connect QR
    code shows up quickly. User reports, October 4: the setup pages only switched after the swipe (and froze while
    the radio switched), and the QR code took ~2 s (a channel scan the connected device doesn't need)."""
    b = ctx.board
    if 'setup1' not in CFG['screens']:
        ctx.note('no "setup1" screen in forge.json: not checked')
        return
    b.show('setup')
    b.wait_screen('setup', 6)
    time.sleep(1)
    at = len(ctx.log.lines())                         # own position (log.mark() is the harness's crash check)
    t0 = time.time()
    b.cmd('swipe left')
    b.wait_screen('setup1', 3)
    m = ctx.log.wait(r'slide: drag: first frame after (\d+) ms, (\d+) frames in (\d+) ms \((\d+) fps\), to next', 5,
                     'the setup page following the finger', start=at)
    fps = int(m.group(4))
    check(fps >= 40, f'setup page drag at {fps} fps')
    ctx.metric('setup_page_fps', fps)
    page = ctx.log.wait(r'ui: Wi-Fi setup page 1', 5, 'Easy Connect started', start=at)
    qr = ctx.log.wait(r'net: Easy Connect: QR code ready', 10, 'the Easy Connect QR code', start=at)
    qr_s = (log_ms(qr.string) - log_ms(page.string)) / 1000
    ctx.metric('easy_connect_qr_s', round(qr_s, 2))
    b.cmd('swipe right')                                # back to the first page, then closed (online again)
    b.wait_screen('setup', 3)
    go_home(ctx)
    back_s = b.wait_online()                            # the Easy Connect page left the home network: Wi-Fi back
    ctx.note(f'setup -> Easy Connect by a drag at {fps} fps, its QR code {qr_s:.2f} s after the page '
             f'({time.time() - t0:.1f} s in all, console round trips included); Wi-Fi back {back_s} s after closing')


@test('navigation')
def moving_press_is_no_long_press(ctx):
    """A finger that moves while held is a swipe, not a long press (L200): LVGL fires LONG_PRESSED after 400 ms however
    far the finger moved when no scroll took over. The start-up message screen has no pager (slide.c's drags don't take
    the press over), and a long press there opens Wi-Fi setup: a slow 70 px drag across it must not (v0.6.0:
    slide_held_still, which also guards the hello page's long press to Settings)."""
    b = ctx.board
    w, h = CFG['screen']['w'], CFG['screen']['h']
    b.show('message')
    b.wait_screen('message', 4)
    try:
        at = len(ctx.log.lines())
        b.cmd(f'drag {w // 2 - 35} {h // 2} {w // 2 + 35} {h // 2} 900')
        time.sleep(1.0)
        # LVGL did fire LONG_PRESSED on the moving finger (so the test proves the guard, not a quiet LVGL)
        ctx.log.wait(r'ui: long-press from a moving finger', 2, 'the long press from the moving finger, ignored', start=at)
        s = b.screen()
        check(s == 'message', f'a 70 px drag held 900 ms on the message screen opened "{s}": a long press from a moving finger')
    finally:
        go_home(ctx)
    ctx.note('a 70 px drag held 900 ms on the start-up message screen (no pager) opened nothing')


def log_ms(line):
    """The ESP-IDF timestamp of a log line, in ms ("I (12345) tag: ...")."""
    m = re.search(r'\((\d+)\)', line)
    return int(m.group(1)) if m else 0


# ---------------------------------------------------------------- perf

def first_minute_wait_s(uptime_s):
    """Seconds to wait so a measurement doesn't fall in the device's first-minute window: forge_ota's update check
    (a TLS handshake) and diag's first report both run ~60 s after boot. The harness's own order put perf.page_swipes
    right there (v0.6.0-fix.1 runs: a 135 ms and a 90 ms LVGL gap, "lvgl>lvgl", both with "ota: checking" in the window)."""
    return 71 - uptime_s if 55 <= uptime_s <= 70 else 0


def measure(ctx, name, action, settle=1.0):
    """Frame rate during `action` (console commands): fps reset, act, wait for the animation to end, read fps.
    anim_fps counts frames less than 250 ms apart; gap_max_ms is the longest wait between two of them."""
    b = ctx.board
    b.wait_online()                                  # never while Wi-Fi rejoins after a setup page (46 fps once)
    wait_s = first_minute_wait_s(b.heap().get('uptime_s', 0))
    if wait_s:                                       # nor during the update check a minute after boot
        ctx.note(f'{name}: waited {wait_s} s for the first-minute update check to pass')
        time.sleep(wait_s)
    if time.localtime().tm_sec > 55:                 # not across a minute change: the clock's redraw right after a
        time.sleep(62 - time.localtime().tm_sec)     # move counts in swipe_gap_max_ms (weather_amoled: 127 ms once)
    b.cmd('fps reset')
    for c in action:
        b.cmd(c)
    time.sleep(settle)
    line = b.cmd('fps', r'test: fps (.*)').group(1)
    kv = dict(x.split('=', 1) for x in line.split() if '=' in x)
    v = {k: float(x) for k, x in kv.items() if re.fullmatch(r'-?[\d.]+', x)}   # gap_max_kind is text
    check(v.get('anim_frames', 0) > 0, f'{name}: no animation frames counted ({line})')
    ctx.metric(f'swipe_fps.{name}', round(v['anim_fps'], 1))
    ctx.metric(f'swipe_gap_max_ms.{name}', round(v['gap_max_ms']))
    ctx.metric(f'swipe_render_avg_ms.{name}', round(v['render_avg_ms'], 1))
    ctx.note(f'{name}: {v["anim_fps"]:.1f} fps, {int(v["anim_frames"])} frames, render avg {v["render_avg_ms"]:.1f} ms '
             f'max {v["render_max_ms"]:.1f} ms, worst gap {v["gap_max_ms"]:.0f} ms' +
             (f' ({kv["gap_max_kind"]}, {kv["gap_max_at_ms"]} ms after the reset)' if 'gap_max_kind' in kv else ''))


@test('perf')
def page_swipes(ctx):
    b = ctx.board
    go_home(ctx)
    at = len(ctx.log.lines())
    measure(ctx, f'{HOME}_to_{NEXT}', ['swipe left'])
    b.wait_screen(NEXT, 4)
    # The neighbour's picture was ready (rendered when idle; v0.6.0: its buffer allocated on demand): the first frame
    # ~15 ms after the 10 px that make it a drag; 35-45 when the picture had to be rendered first
    m = ctx.log.wait(r'slide: drag: first frame after (\d+) ms', 3, 'the drag line', start=at)
    ctx.metric('drag_first_frame_ms', int(m.group(1)))
    measure(ctx, f'{NEXT}_to_{HOME}', ['swipe right'])
    b.wait_screen(HOME, 4)
    w, h = CFG['screen']['w'], CFG['screen']['h']        # a slow drag, finger-following, then the snap
    measure(ctx, 'drag_slow', [f'drag {w * 3 // 4} {h // 2} {w // 4} {h // 2} 600'], settle=1.5)
    b.wait_screen(NEXT, 4)
    go_home(ctx)


@test('perf')
def settings_scroll(ctx):
    """The Settings list scrolled by a finger, drawn as pictures from the screen's shadow (v0.6.0, weather_amoled's
    slide_scroll): rows moved within the picture, the rows coming in rendered, only the list's rectangle sent. LVGL's own
    scrolling of it ran at ~25 fps. Fails on the old firmware: no "slide: scroll" line."""
    b = ctx.board
    go_home(ctx)
    b.press()
    b.wait_screen('settings', 6)
    time.sleep(1.0)
    w, h = CFG['screen']['w'], CFG['screen']['h']
    at = len(ctx.log.lines())
    try:
        measure(ctx, 'settings_scroll', [f'drag {w // 2} {h * 3 // 4} {w // 2} {h // 4} 400'], settle=1.5)
        m = ctx.log.wait(r'slide: scroll: (\d+) frames in (\d+) ms', 3, 'the list scrolled as pictures', start=at)
        ctx.note(f'settings scrolled as pictures: {m.group(1)} frames in {m.group(2)} ms')
    finally:
        b.tap(SCREEN_C[0], 42)                               # Done
        b.wait_screen(HOME, 6)


# ---------------------------------------------------------------- presence (forge_presence: screen dimming)

def presence_state(b):
    m = re.search(r'state=(\d) brightness=(\d+) .*mic=(\d)', b.cmd('presence', r'test: presence (.*)').group(1))
    return int(m.group(1)), int(m.group(2)), m.group(3) == '1'


@test('presence')
def dim_off_wake(ctx):
    """Short delays through the API (the user's settings put back after): ACTIVE -> DIM -> OFF, then a long-press on
    the dark screen only wakes it: the board's press filter (presence_touch) swallows it, so setup doesn't open
    (esp32-s3-rtcquebec v0.3.0, the touch that wakes must not also act)."""
    b = ctx.board
    saved = b.api('/api/presence')
    keys = ('enabled', 'margin_db', 'wake_s', 'dim_s', 'off_s', 'bright_pct', 'dim_pct', 'motion_wake')
    orig = {k: saved[k] for k in keys}
    try:
        go_home(ctx)
        state, _, mic = presence_state(b)
        check(mic, 'the microphones are not available (board_mic_open): presence never runs')
        # loud never (60 dB over the background), no pick-up: only time decides
        at = len(ctx.log.lines())
        b.api('/api/presence', {'enabled': True, 'margin_db': 60, 'dim_s': 2, 'off_s': 2, 'motion_wake': False})
        ctx.log.wait(r'presence: ACTIVE -> DIM', 15, 'dims after 2 s of quiet', start=at)
        ctx.log.wait(r'presence: DIM -> OFF', 15, 'off 2 s later', start=at)
        # v0.6.0: off is a real off (the panel asleep, the "screen" lock released so an idle core may drop to 80 MHz):
        # the fade's last step puts it to sleep (forge_presence's screen_off hook, the board's display_sleep)
        ctx.log.wait(r'display: panel asleep', 6, 'the panel asleep once the fade is over (screen_off hook)', start=at)
        time.sleep(1.5)                                  # faded out
        state, bright, _ = presence_state(b)
        check(state == 2 and bright == 0, f'not off: state {state}, brightness {bright}')
        p = b.power()
        check(p.get('panel') == 'asleep', f'screen off but the panel {p.get("panel")}')
        check(p.get('screen_lock') == '0', 'screen off but the CPU held at 240 MHz (the "screen" lock)')
        at = len(ctx.log.lines())
        b.press()                                        # a long-press: would open setup on a lit screen
        ctx.log.wait(r'presence: touch on a dark screen', 6, 'the press filter woke it', start=at)
        ctx.log.wait(r'display: panel awake', 6, 'the panel awake on the first step of the fade up', start=at)
        time.sleep(1.5)
        state, bright, _ = presence_state(b)
        check(state == 0 and bright > 0, f'not awake: state {state}, brightness {bright}')
        check(b.screen() == HOME, f'the waking long-press also acted: on {b.screen()}, not {HOME}')
        p = b.power()
        check(p.get('panel') == 'awake' and p.get('screen_lock') == '1',
              f'awake but the panel {p.get("panel")}, the "screen" lock {p.get("screen_lock")} (1 expected: 240 MHz)')
        ctx.note('dim after 2 s, off 2 s later (panel asleep, the CPU free to idle slower); a long-press on the dark '
                 'screen only woke it (panel awake, 240 MHz held)')
    finally:
        b.api('/api/presence', orig)
        b.cmd('wake')

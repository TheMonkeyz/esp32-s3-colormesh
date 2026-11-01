"""ColorMesh's own tests (main/: the pager's pick ring | main | system pages, the Bluetooth mesh through the console).
Same registry (@test from board.py), same ctx as core_suites.py. The board has no network: every test goes through
the USB console and the serial log (docs/PROTOCOL.md; the commands: color, color2, devmode, name, bright, peers, cfg,
hold, shot, plus forge_lvgl's screen / tap / press / swipe / drag and the board's fps).

    APP_ORDER       the app's suites, run after the core start-up checks (harness.py)
    APP_WATCH       log lines that aren't failures but must not go unseen: (regex, what), counted per test
    METRIC_SUITES   which suite produces a metric (prefix -> suite): a baseline metric is only expected when its
                    suite ran

Screens (pick, main, system, message) are snapshotted by the core `screens` suite (Board.snap -> `shot` over USB,
metrics snapshot_ms.<name>): not registered again here.
"""
import math
import re
import time

from board import CFG, SCREEN_C, check, test

APP_ORDER = ['navigation', 'mesh', 'perf']
APP_WATCH = [
    (r'display: .*did not finish', 'a panel transfer timed out'),
    (r'lvgl: .*(out of memory|alloc failed)', 'LVGL could not allocate'),
    (r'^[WE] \(\d+\) ble: ', 'a Bluetooth warning or error line'),
    (r'^[WE] \(\d+\) mesh: ', 'a mesh warning or error line'),
    (r'fw: .*(fail|abort|error)', 'a firmware pull that did not finish'),
]
METRIC_SUITES = [('drag_first_frame_ms', 'perf'), ('swipe_', 'perf')]

PAGES = ['pick', 'main', 'system']                     # the pager's pages, left to right: ring | main | system
PREV, HOME, NEXT = PAGES                               # a swipe right on main opens the ring, a swipe left system
RING_R = 180                                           # the pick page: 12 swatches on a ring around the centre,
RED_SWATCH = (SCREEN_C[0], SCREEN_C[1] - RING_R)       # hue 0 = red at the top, clockwise every 30 degrees
RED = '#ff0000'


def swatch_at(i):
    """The centre of swatch i (0 = red at the top, clockwise every 30 degrees), as ui.c places it."""
    a = math.radians(i * 30)
    return SCREEN_C[0] + round(RING_R * math.sin(a)), SCREEN_C[1] - round(RING_R * math.cos(a))


GREEN_SWATCH = swatch_at(4)                            # 120 degrees: #00ff00, (389, 323)
GREEN = '#00ff00'
LONG_PRESS_MS = 800                                    # the console's still finger; LVGL's long press is 400 ms


def go_home(ctx):
    ctx.board.show(HOME)
    ctx.board.wait_screen(HOME, 6)
    time.sleep(0.5)


def log_ms(line):
    """The ESP-IDF timestamp of a log line, in ms ("I (12345) tag: ...")."""
    m = re.search(r'\((\d+)\)', line)
    return int(m.group(1)) if m else 0


def put_back_color(b, saved):
    """The colour the board had before a test set its own (the user's choice, kept in NVS)."""
    if saved and re.fullmatch(r'#[0-9a-fA-F]{6}', saved) and saved.lower() != RED:
        b.color(saved[1:])


COLOR2_OK = r'test: ok color2 (#[0-9a-f]{6}|auto)'


def set_color2(b, value):
    """`color2 RRGGBB|auto` -> what the board says it set: '#rrggbb' or 'auto'."""
    return b.cmd(f'color2 {value.lstrip("#")}', COLOR2_OK).group(1)


def put_back_color2(b, saved, now):
    """The wave colour the board had before a test (the user's choice, kept in NVS), unless it is that already."""
    if saved and saved != now and (saved == 'auto' or re.fullmatch(r'#[0-9a-fA-F]{6}', saved)):
        set_color2(b, saved)


# ---------------------------------------------------------------- navigation

@test('navigation')
def pick_then_main(ctx):
    """The pick ring: a tap on the red swatch (top of the ring) saves red (log `cfg: color #ff0000`, `cfg` agrees)
    and shows main. The user's colour is put back after."""
    b = ctx.board
    saved = b.cfg().get('color')
    try:
        b.show('pick')
        b.wait_screen('pick', 6)
        time.sleep(0.5)
        at = len(ctx.log.lines())
        b.tap(*RED_SWATCH)
        ctx.log.wait(r'cfg: color ' + RED, 4, 'the red swatch tapped', start=at)
        b.wait_screen(HOME, 6)
        c = b.cfg().get('color')
        check(c == RED, f'after the tap on red, cfg says color {c}')
    finally:
        put_back_color(b, saved)
    ctx.note(f'pick: a tap at {RED_SWATCH} set {RED} and opened {HOME}; the colour {saved} put back')


@test('navigation')
def pick_long_press(ctx):
    """The pick ring: a long press (the console's still finger, LONG_PRESS_MS) on the green swatch makes green the
    waves' colour B (`cfg: color2 #00ff00`, `cfg` agrees) without being a tap (no `cfg: color #`, the ring stays
    shown); a second long press on it goes back to automatic (`cfg: color2 auto`). Both choices are put back."""
    b = ctx.board
    before = b.cfg()
    saved2, colour = before.get('color2'), before.get('color')
    check(saved2 is not None, '"cfg" has no "color2": a firmware without the wave colour')
    now = saved2
    try:
        if saved2 != 'auto':
            now = set_color2(b, 'auto')
        b.show('pick')
        b.wait_screen('pick', 6)
        time.sleep(0.5)
        at = len(ctx.log.lines())
        b.press(*GREEN_SWATCH, LONG_PRESS_MS)
        ctx.log.wait(r'cfg: color2 ' + GREEN, 3, 'the long press on the green swatch', start=at)
        now = b.cfg().get('color2')
        check(now == GREEN, f'after the long press on green, cfg says color2 {now}')
        time.sleep(0.8)                                # a tap would have slid to main by now (~0.4 s)
        shown = b.screen()
        check(shown == 'pick', f'the long press also left the ring: the screen is {shown}')
        at2 = len(ctx.log.lines())
        b.press(*GREEN_SWATCH, LONG_PRESS_MS)
        ctx.log.wait(r'cfg: color2 auto', 3, 'the second long press on green (back to automatic)', start=at2)
        now = b.cfg().get('color2')
        check(now == 'auto', f'after the second long press, cfg says color2 {now}')
        taps = [l for l in ctx.log.lines()[at:] if re.search(r'cfg: color #', l)]
        check(not taps, f'a long press counted as a tap too: {taps[0] if taps else ""}')
        after = b.cfg().get('color')
        check(after == colour, f'the long presses changed the colour: {colour} -> {after}')
    finally:
        put_back_color2(b, saved2, now)
    ctx.note(f'pick: a long press at {GREEN_SWATCH} set color2 {GREEN}, a second one auto; the ring stayed, the '
             f'colour {colour} kept; color2 {saved2} put back')


@test('navigation')
def swipe_to_system(ctx):
    """A swipe left from main shows system; a swipe right comes back."""
    b = ctx.board
    go_home(ctx)
    b.cmd('swipe left')
    time.sleep(0.8)
    b.wait_screen(NEXT, 6)
    b.cmd('swipe right')
    time.sleep(0.8)
    b.wait_screen(HOME, 6)
    ctx.note(f'{HOME} <-> {NEXT} by swipes')


@test('navigation')
def swipe_to_ring(ctx):
    """A swipe right from main opens the colour ring (`pick`); a swipe left goes back to main without changing the
    colour (only a tap on a swatch saves one: no `cfg: color` line, `cfg` agrees)."""
    b = ctx.board
    before = b.cfg().get('color')
    go_home(ctx)
    at = len(ctx.log.lines())
    b.cmd('swipe right')
    time.sleep(0.8)
    b.wait_screen(PREV, 6)
    b.cmd('swipe left')
    time.sleep(0.8)
    b.wait_screen(HOME, 6)
    saved = [l for l in ctx.log.lines()[at:] if re.search(r'cfg: color ', l)]
    check(not saved, f'the swipes {HOME} -> {PREV} -> {HOME} saved a colour: {saved[0] if saved else ""}')
    after = b.cfg().get('color')
    check(after == before, f'the swipes {HOME} -> {PREV} -> {HOME} changed the colour: {before} -> {after}')
    ctx.note(f'{HOME} -> {PREV} -> {HOME} by swipes, the colour {before} kept')


# ---------------------------------------------------------------- mesh (Bluetooth, through the console)

@test('mesh')
def hold_sends(ctx):
    """`hold on` (the simulated finger) is logged `mesh: hold on` within 1 s, `hold off` as `mesh: hold off`: the
    mesh task picks the UI's flag up at once (the advert changes with it)."""
    b = ctx.board
    go_home(ctx)
    try:
        at = b.hold(True)
        on = ctx.log.wait(r'mesh: hold on', 1, '"hold on" in the log', start=at)
        at = b.hold(False)
        off = ctx.log.wait(r'mesh: hold off', 1, '"hold off" in the log', start=at)
    finally:
        b.hold(False)
    ctx.note(f'hold on / off logged at {log_ms(on.string)} / {log_ms(off.string)} ms')


SWIPE_HOLD_MS = 100                                    # a swipe from main: "hold off" at most this long after "on"


def swipe_hold(lines, page):
    """The hold a swipe from main sent, from the lines logged since the swipe: (ms from `mesh: hold on` to the next
    `mesh: hold off`, whether that `hold off` came before `ui: page <page>`). (None, True) when the swipe sent no
    hold; (None, False) when the hold never ended in these lines."""
    on = off = settled = None
    for i, line in enumerate(lines):
        if on is None and re.search(r'mesh: hold on\b', line):
            on = i
        elif on is not None and off is None and re.search(r'mesh: hold off\b', line):
            off = i
        if settled is None and re.search(r'ui: page ' + re.escape(page) + r'\b', line):
            settled = i
    if on is None:
        return None, True
    if off is None:
        return None, False
    return log_ms(lines[off]) - log_ms(lines[on]), settled is None or off < settled


@test('mesh')
def swipe_no_hold(ctx):
    """A swipe from main ends the hold its press sent at the drag's start (slide.c's drag hook: `ui: hold released
    (drag)`): `mesh: hold off` within SWIPE_HOLD_MS of `mesh: hold on`, before the page settles, both ways (to system
    and to the ring). Before the hook the hold lasted the whole slide (~0.4 s): the other boards flashed on every swipe."""
    b = ctx.board
    seen = []
    for swipe, page in (('swipe left', NEXT), ('swipe right', PREV)):
        go_home(ctx)
        at = len(ctx.log.lines())
        b.cmd(swipe)
        ctx.log.wait(r'ui: page ' + page, 6, f'"{swipe}" from {HOME} settling on {page}', start=at)
        time.sleep(0.5)                                    # the safety net's release, if the hook did not run
        gap, before = swipe_hold(ctx.log.lines()[at:], page)
        check(before, f'"{swipe}" from {HOME}: "mesh: hold off" ' +
              ('never logged' if gap is None else f'only after "ui: page {page}" ({gap} ms after "hold on")'))
        check(gap is None or gap <= SWIPE_HOLD_MS,
              f'"{swipe}" from {HOME}: held {gap} ms (limit {SWIPE_HOLD_MS} ms)')
        seen.append(f'{swipe} -> {page}: ' + ('no hold' if gap is None else f'held {gap} ms'))
    go_home(ctx)
    ctx.note('; '.join(seen))


@test('mesh')
def peers_seen(ctx):
    """`peers`: n=0 on a single board (passes with a note); else one `test: peer` line per peer, and `cfg` agrees."""
    b = ctx.board
    n, peers = b.peers()
    if n == 0:
        ctx.note('peers: n=0 (a single board, nobody advertising): nothing to compare')
        return
    check(len(peers) == n, f'"peers" said n={n} but {len(peers)} "test: peer" lines followed')
    in_cfg = b.cfg().get('peers') or []
    check(len(in_cfg) == n, f'"cfg" lists {len(in_cfg)} peers, "peers" says {n}')
    ctx.note(f'{n} peer(s): ' + '; '.join(f'{p["addr"]} {p["ver"]} {p["color"]} {"hold" if p["holding"] else "idle"} '
                                         f'rssi={p["rssi"]}' for p in peers))


@test('mesh')
def mix_self(ctx):
    """This board's own colour mixes in while it holds: with `color ff0000` and `hold on`, the first `mesh: mix` line
    is `#ff0000 n=1` (not checked while a peer is holding: n would not be 1)."""
    b = ctx.board
    n, peers = b.peers()
    if any(p['holding'] for p in peers):
        ctx.note('a peer is holding: mix_self not checked (n=1 impossible)')
        return
    saved = b.cfg().get('color')
    try:
        go_home(ctx)
        b.color('ff0000')
        at = b.hold(True)
        m = ctx.log.wait(r'mesh: mix #([0-9a-fA-F]{6}) n=(\d+)', 3, 'the first mix line after "hold on"', start=at)
        check((m.group(1).lower(), m.group(2)) == ('ff0000', '1'),
              f'first mix after hold on: #{m.group(1)} n={m.group(2)}, expected #ff0000 n=1')
    finally:
        b.hold(False)
        put_back_color(b, saved)
    ctx.note(f'own colour #ff0000 held alone: mix #ff0000 n=1 at {log_ms(m.string)} ms; the colour {saved} put back')


WAVE_FPS_MIN = 20                                      # a wave on main: frames per second, at least
WAVE_LINE = (r'display: wave frames (\d+), fill avg ([\d.]+) max ([\d.]+) ms, frame avg ([\d.]+) max ([\d.]+) ms, '
             r'([\d.]+) fps \((\w+)\)')


def wave_stats(line):
    """`display: wave frames N, fill avg A max B ms, frame avg C max D ms, F fps (end|paused)` (ui.c's wave loop, at
    each stop) -> dict, or None for another line."""
    m = re.search(WAVE_LINE, line)
    if not m:
        return None
    return {'frames': int(m.group(1)), 'fill_avg': float(m.group(2)), 'fill_max': float(m.group(3)),
            'frame_avg': float(m.group(4)), 'frame_max': float(m.group(5)), 'fps': float(m.group(6)),
            'why': m.group(7)}


@test('mesh')
def wave_tap(ctx):
    """A tap at the centre of main starts a centre wave (`ui: wave centre at 233,233`, then `mesh: wave centre from
    self`, both within 1 s); the main page draws it as raw frames and, when it ends (WAVE_DURATION_MS, 4 s), logs
    `display: wave frames ...` with at least WAVE_FPS_MIN fps (within 5 s of the tap)."""
    b = ctx.board
    go_home(ctx)
    at = len(ctx.log.lines())
    b.tap(*SCREEN_C)
    ui = ctx.log.wait(r'ui: wave centre at \d+,\d+', 1, 'the tap at the centre taken as a wave', start=at)
    ctx.log.wait(r'mesh: wave centre from self', 1, 'the mesh starting the own wave', start=at)
    m = ctx.log.wait(WAVE_LINE, 5, "the wave loop's frame line at the end of the wave", start=at)
    v = wave_stats(m.string)
    check(v['why'] == 'end', f'the wave stopped before its end ({v["why"]}): {m.string.strip()}')
    check(v['fps'] >= WAVE_FPS_MIN, f'wave at {v["fps"]:.1f} fps (at least {WAVE_FPS_MIN}): {m.string.strip()}')
    ctx.note(f'tap at the centre -> {ui.group(0)}; {v["frames"]} frames at {v["fps"]:.1f} fps, fill avg '
             f'{v["fill_avg"]:.1f} max {v["fill_max"]:.1f} ms, frame avg {v["frame_avg"]:.1f} max {v["frame_max"]:.1f} ms')


@test('mesh')
def color2(ctx):
    """The waves' colour B over the console: `color2 00ff00` -> `test: ok color2 #00ff00` (log `cfg: color2
    #00ff00`) and `cfg` says "color2":"#00ff00"; `color2 auto` -> `test: ok color2 auto` and `cfg` says "auto". The
    user's choice is put back."""
    b = ctx.board
    saved = b.cfg().get('color2')
    check(saved is not None, '"cfg" has no "color2": a firmware without the wave colour')
    now = saved
    try:
        at = len(ctx.log.lines())
        now = set_color2(b, '00ff00')
        check(now == GREEN, f'"color2 00ff00" answered {now}')
        ctx.log.wait(r'cfg: color2 ' + GREEN, 2, 'the config line of "color2 00ff00"', start=at)
        c = b.cfg().get('color2')
        check(c == GREEN, f'after "color2 00ff00", cfg says color2 {c}')
        at = len(ctx.log.lines())
        now = set_color2(b, 'auto')
        check(now == 'auto', f'"color2 auto" answered {now}')
        ctx.log.wait(r'cfg: color2 auto', 2, 'the config line of "color2 auto"', start=at)
        c = b.cfg().get('color2')
        now = c
        check(c == 'auto', f'after "color2 auto", cfg says color2 {c}')
    finally:
        put_back_color2(b, saved, now)
    ctx.note(f'color2 {GREEN} then auto, both in cfg; the wave colour {saved} put back')


# ---------------------------------------------------------------- perf

def measure(ctx, name, action, settle=1.0):
    """Frame rate during `action` (console commands): fps reset, act, wait for the animation to end, read fps.
    anim_fps counts frames less than 250 ms apart; gap_max_ms is the longest wait between two of them."""
    b = ctx.board
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
    """main -> system and back by swipes, then a slow finger-following drag: fps, the worst gap, the render average
    and the drag's first frame against the baseline."""
    b = ctx.board
    go_home(ctx)
    at = len(ctx.log.lines())
    measure(ctx, f'{HOME}_to_{NEXT}', ['swipe left'])
    b.wait_screen(NEXT, 4)
    # The neighbour's picture was ready (rendered when idle): the first frame ~15 ms after the 10 px that make it a
    # drag; 35-45 when the picture had to be rendered first
    m = ctx.log.wait(r'slide: drag: first frame after (\d+) ms', 3, 'the drag line', start=at)
    ctx.metric('drag_first_frame_ms', int(m.group(1)))
    measure(ctx, f'{NEXT}_to_{HOME}', ['swipe right'])
    b.wait_screen(HOME, 4)
    w, h = CFG['screen']['w'], CFG['screen']['h']        # a slow drag, finger-following, then the snap
    measure(ctx, 'drag_slow', [f'drag {w * 3 // 4} {h // 2} {w // 4} {h // 2} 600'], settle=1.5)
    b.wait_screen(NEXT, 4)
    go_home(ctx)

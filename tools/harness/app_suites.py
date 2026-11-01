"""ColorMesh's own tests (main/: the pick ring, the main and system pages, the Bluetooth mesh through the console).
Same registry (@test from board.py), same ctx as core_suites.py. The board has no network: every test goes through
the USB console and the serial log (docs/PROTOCOL.md; the commands: color, devmode, name, bright, peers, cfg, hold,
shot, plus forge_lvgl's screen / tap / press / swipe / drag and the board's fps).

    APP_ORDER       the app's suites, run after the core start-up checks (harness.py)
    APP_WATCH       log lines that aren't failures but must not go unseen: (regex, what), counted per test
    METRIC_SUITES   which suite produces a metric (prefix -> suite): a baseline metric is only expected when its
                    suite ran

Screens (pick, main, system, message) are snapshotted by the core `screens` suite (Board.snap -> `shot` over USB,
metrics snapshot_ms.<name>): not registered again here.
"""
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

HOME, NEXT = 'main', 'system'                          # the pager's two pages, side by side: main | system
RING_R = 180                                           # the pick screen: 12 swatches on a ring around the centre,
RED_SWATCH = (SCREEN_C[0], SCREEN_C[1] - RING_R)       # hue 0 = red at the top, clockwise every 30 degrees
RED = '#ff0000'


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

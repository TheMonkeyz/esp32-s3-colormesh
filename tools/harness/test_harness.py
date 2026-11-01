"""Unit tests of the harness's own logic (no board):   python tools/harness/test_harness.py
(or python -m unittest discover -s tools/harness -p "test_*.py")

ColorMesh has no network (forge.json "no_net"): the no-net mode is tested here (the argument parsing, the boot check on
the diag lines, the console expectation, the `shot` block decoder in tools/shot.py and Board.snap over it), next to
the generic logic (baseline comparison, forge.json, staging). The twin behaviour (a board on a network) is tested
with harness.NO_NET / board no_net set to False.
"""
import base64
import contextlib
import io
import json
import os
import re
import shutil
import struct
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..'))
import forgecfg  # noqa: E402
import shot  # noqa: E402  (tools/shot.py)
from board import BOOT_SEEN, CFG, SUITES, Board, Fail, Shot, english_netsh, kv  # noqa: E402
import board  # noqa: E402
import core_suites  # noqa: E402
import harness  # noqa: E402
from harness import BAD, BASELINE, NET_SUITES, ORDER, compare, parse_args, propose, suite_of  # noqa: E402
import app_suites  # noqa: E402

BASE = {
    '_comment': 'not a metric',
    'swipe_fps.hello_to_system': {'min': 20, 'ref': 45, 'note': 'kept'},
    'internal_min_kb': {'min': 8, 'ref': 9},
    'snapshot_ms.hello': {'max': 5000, 'ref': 2300},
    'swipe_gap_max_ms.hello_to_system': {'max': 200, 'ref': 60, 'dio': {'max': 260}},
}


def load_json(path):
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def write(path, data):
    with open(path, 'wb' if isinstance(data, bytes) else 'w') as f:
        f.write(data if isinstance(data, (bytes, str)) else json.dumps(data))


def verdicts(rows):
    return {k: v for k, _, _, v in rows}


def snapshot_screens(base):
    """The screens the baseline has snapshot times for; None for a new project's empty baseline."""
    if not base:
        return None
    return {k.split('.', 1)[1] for k in base if k.startswith('snapshot_ms.')}


@contextlib.contextmanager
def net_mode(no_net):
    """harness.NO_NET as a project with / without a network has it (forge.json no_net), put back after."""
    old = harness.NO_NET
    harness.NO_NET = no_net
    try:
        yield
    finally:
        harness.NO_NET = old


class Compare(unittest.TestCase):
    def test_within_and_outside_limits(self):
        v = verdicts(compare({'swipe_fps.hello_to_system': 30, 'swipe_gap_max_ms.hello_to_system': 250,
                              'internal_min_kb': 7}, BASE, ['perf', 'memory']))
        self.assertEqual(v['swipe_fps.hello_to_system'], 'ok')
        self.assertEqual(v['swipe_gap_max_ms.hello_to_system'], 'REGRESSION')
        self.assertEqual(v['internal_min_kb'], 'REGRESSION')

    def test_dio_limits(self):
        v = verdicts(compare({'swipe_fps.hello_to_system': 30, 'swipe_gap_max_ms.hello_to_system': 250}, BASE,
                             ['perf'], mode='DIO'))
        self.assertEqual(v['swipe_gap_max_ms.hello_to_system'], 'ok')

    def test_missing_metric_fails(self):
        # in the baseline, its suite ran, not measured: a regex drift or a reused log window, not a pass
        v = verdicts(compare({'swipe_fps.hello_to_system': 40}, BASE, ['perf', 'memory']))
        self.assertEqual(v['internal_min_kb'], 'MISSING')
        self.assertIn('MISSING', BAD)

    def test_missing_from_a_suite_that_did_not_run(self):
        v = verdicts(compare({'internal_min_kb': 9}, BASE, ['memory']))
        self.assertNotIn('snapshot_ms.hello', v)              # screens didn't run
        self.assertNotIn('_comment', v)
        v = verdicts(compare({}, BASE, ['screens']))
        self.assertEqual(v, {'snapshot_ms.hello': 'MISSING'})

    def test_metric_without_a_limit_fails(self):
        v = verdicts(compare({'internal_min_kb': 9, 'swipe_fps.new': 20}, BASE, ['memory']))
        self.assertEqual(v['swipe_fps.new'], 'NEW')
        self.assertIn('NEW', BAD)

    def test_explicit_skip(self):
        v = verdicts(compare({'swipe_fps.hello_to_system': 40, 'swipe_gap_max_ms.hello_to_system': 20}, BASE,
                             ['perf', 'memory'], {'internal_*': 'not today'}))
        self.assertEqual(v['internal_min_kb'], 'skipped: not today')
        self.assertNotIn(v['internal_min_kb'], BAD)

    def test_propose_keeps_limits_and_notes(self):
        out = propose({'swipe_fps.hello_to_system': 47, 'page_kb': 53, 'swipe_fps.x': 60, 'idle_internal_drop_kb': 2},
                      BASE)
        self.assertEqual(out['swipe_fps.hello_to_system'], {'min': 20, 'ref': 47, 'note': 'kept'})
        self.assertIn('max', out['page_kb'])                  # bigger is worse, though it ends in _kb
        self.assertIn('max', out['idle_internal_drop_kb'])
        self.assertIn('min', out['swipe_fps.x'])
        self.assertEqual(BASE['swipe_fps.hello_to_system']['ref'], 45)  # the input is not changed

    def test_propose_dio(self):
        out = propose({'swipe_gap_max_ms.hello_to_system': 90}, BASE, mode='DIO')
        self.assertEqual(out['swipe_gap_max_ms.hello_to_system']['dio']['ref'], 90)
        self.assertEqual(out['swipe_gap_max_ms.hello_to_system']['ref'], 60)


class Baseline(unittest.TestCase):
    def test_every_baseline_metric_has_a_suite(self):
        # a metric no suite claims would never be MISSING: it could disappear unnoticed
        base = load_json(BASELINE)
        for k in base:
            if not k.startswith('_'):
                self.assertIn(suite_of(k), SUITES, f'{k}: no suite produces it (METRIC_SUITES)')

    def test_every_suite_is_ordered(self):
        for s in SUITES:
            self.assertIn(s, ORDER)

    def test_snapshot_metrics_follow_forge_screens(self):
        snaps = snapshot_screens(load_json(BASELINE))
        if snaps is None:
            self.skipTest('baseline.json is {} (a new project): the first harness run writes it (docs/NEW-PROJECT.md)')
        self.assertEqual(snaps, set(CFG['screens']))

    def test_a_new_projects_empty_baseline_is_skipped(self):
        # NEW-PROJECT resets the baseline to {}: CI failed on it until the first harness run (esp32-s3-rtcquebec)
        self.assertIsNone(snapshot_screens({}))
        self.assertEqual(snapshot_screens({'snapshot_ms.home': 90, 'fps.x': 1}), {'home'})

    def test_app_metrics_have_their_suite(self):
        # what app_suites measures: perf's swipe_* and drag_first_frame_ms; snapshots are the core screens suite's
        self.assertEqual(suite_of('swipe_fps.main_to_system'), 'perf')
        self.assertEqual(suite_of('drag_first_frame_ms'), 'perf')
        self.assertEqual(suite_of('snapshot_ms.pick'), 'screens')
        for s in app_suites.APP_ORDER:
            self.assertIn(s, SUITES, f'{s} in APP_ORDER but no test registered')


class Args(unittest.TestCase):
    """The twin behaviour, a project on a network (harness.NO_NET False), whatever this forge.json says."""

    def test_default_runs_all_but_ota(self):
        with net_mode(False):
            o = parse_args([])
        self.assertNotIn('ota', o.run)
        self.assertIn('boot', o.run)
        self.assertIn('web', o.run)
        self.assertFalse(o.no_net)

    def test_quick_leaves_out_the_slow_suites(self):
        with net_mode(False):
            o = parse_args(['--quick'])
            self.assertNotIn('idle_stable', o.run)
            self.assertNotIn('wifi_setup', o.run)
            self.assertIn('web', o.run)                      # the web suite's device tests stay; Playwright doesn't
            self.assertEqual(parse_args(['--quick', 'idle_stable']).run, ['idle_stable'])   # asked for: run

    def test_ota_flag_adds_the_suite_and_expect(self):
        with net_mode(False):
            o = parse_args(['--ota', 'v1.2.0-rc.1'])
        self.assertIn('ota', o.run)
        self.assertEqual(o.expect, 'v1.2.0-rc.1')

    def test_suite_option_and_flash_default(self):
        with net_mode(False):
            o = parse_args(['--suite', 'boot,console', '--flash'])
            self.assertEqual(o.run, ['boot', 'console'])
            self.assertEqual(o.flash, CFG['build_dir'])
            o = parse_args(['--flash', 'boot'])              # a suite name after --flash is a suite
        self.assertEqual((o.run, o.flash), (['boot'], CFG['build_dir']))

    def test_wifi_setup_only_by_default_on_windows(self):
        # netsh, and a PC that stays online over Ethernet while its Wi-Fi is on the setup network (docs/MACOS.md)
        old = harness.WINDOWS
        try:
            with net_mode(False):
                harness.WINDOWS = True
                self.assertIn('wifi_setup', parse_args([]).run)
                harness.WINDOWS = False
                self.assertNotIn('wifi_setup', parse_args([]).run)
                self.assertIn('wifi_runtime', parse_args([]).run)
                self.assertEqual(parse_args(['wifi_setup']).run, ['wifi_setup'])   # still there when asked for
        finally:
            harness.WINDOWS = old


class NoNetArgs(unittest.TestCase):
    """forge.json "no_net": true (this project), or --no-net: no IP, no network suite, the app's suites in order."""

    def test_the_network_suites_never_run(self):
        with net_mode(True):
            o = parse_args([])
        self.assertTrue(o.no_net)
        self.assertFalse(NET_SUITES & set(o.run), o.run)
        self.assertEqual([s for s in o.run if s in ('boot', 'console', 'memory', 'screens', 'idle_stable')],
                         ['boot', 'console', 'memory', 'screens', 'idle_stable'])
        for s in app_suites.APP_ORDER:
            self.assertIn(s, o.run)
        self.assertEqual(NET_SUITES, {'web', 'update', 'wifi_runtime', 'wifi_setup', 'ota'})

    def test_quick_still_drops_the_idle_minute(self):
        with net_mode(True):
            self.assertNotIn('idle_stable', parse_args(['--quick']).run)

    def test_a_network_suite_asked_for_is_refused(self):
        with net_mode(True), contextlib.redirect_stderr(io.StringIO()):
            for argv in (['web'], ['--suite', 'boot,ota', '--ota', 'v1'], ['wifi_setup']):
                with self.assertRaises(SystemExit, msg=argv):
                    parse_args(argv)

    def test_ip_and_ota_are_refused(self):
        with net_mode(True), contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                parse_args(['--ip', '192.168.1.5'])
            with self.assertRaises(SystemExit):
                parse_args(['--ota', 'v0.1.0-rc.1'])

    def test_the_flag_on_a_network_project(self):
        with net_mode(False):
            o = parse_args(['--no-net'])
            self.assertTrue(o.no_net)
            self.assertFalse(NET_SUITES & set(o.run))
            self.assertEqual(parse_args(['--no-net', 'boot', 'mesh']).run, ['boot', 'mesh'])

    def test_board_without_network_refuses_https(self):
        b = type('B', (), {'no_net': True})()
        for call in (lambda: Board.wait_online(b), lambda: Board.api(b, '/api/info'), lambda: Board.key(b),
                     lambda: Board.request(b, 'GET', '/'), lambda: Board.install(b, 'v1')):
            with self.assertRaises(Fail) as e:
                call()
            self.assertIn('no network', str(e.exception))
        self.assertIsNone(Board.version_before_restart(b))   # nothing to ask before a restart (the helper is idle)
        self.assertRegex('I (4952) diag: running from ota_0', BOOT_SEEN)
        self.assertRegex('I (4952) ota: Running v0.1.1 from ota_0', BOOT_SEEN)


# ---------------------------------------------------------------- the shot block (tools/shot.py)

def row565(*values):
    """A `S:` line: RGB565 pixels, little-endian, base64."""
    return 'S:' + base64.b64encode(b''.join(struct.pack('<H', v) for v in values)).decode()


ROW0 = row565(0xF800, 0x07E0, 0x001F, 0xFFFF)          # red, green, blue, white
ROW1 = row565(0x0000, 0x8410, 0xF81F, 0x07FF)          # black, grey, magenta, cyan
RGB0 = bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255])
RGB1 = bytes([0, 0, 0, 131, 129, 131, 255, 0, 255, 0, 255, 255])    # 0x8410: 16/31, 32/63, 16/31 of 255
SHOT_LINES = ['I (3000) test: ok screen pick', '> shot pick 2', 'SHOT pick 4 2', ROW0, 'W (3010) ble: a line in between',
              ROW1, 'SHOTEND']


def png_parts(data):
    """(w, h, bit depth, colour type, decompressed IDAT) of a PNG."""
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    w, h, depth, ctype = struct.unpack('>IIBB', data[16:26])
    pos, idat = 8, b''
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        if kind == b'IDAT':
            idat += data[pos + 8:pos + 8 + n]
        pos += 12 + n
    return w, h, depth, ctype, zlib.decompress(idat)


class ShotDecoder(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_block_to_pixels(self):
        got = list(shot.blocks(SHOT_LINES))
        self.assertEqual(len(got), 1)
        name, w, h, rgb, rows = got[0]
        self.assertEqual((name, w, h, rows), ('pick', 4, 2, 2))      # the stray log line is not a row
        self.assertEqual(rgb, RGB0 + RGB1)
        self.assertEqual(shot.rgb565(0x8410), (131, 129, 131))
        self.assertEqual(shot.rgb565(0xFFFF), (255, 255, 255))
        self.assertEqual(shot.last_block(SHOT_LINES, 'pick')[0], 'pick')
        self.assertIsNone(shot.last_block(SHOT_LINES, 'main'))
        self.assertIsNone(shot.last_block(SHOT_LINES[:-1]))          # no SHOTEND yet: not complete

    def test_a_garbled_row_is_black_and_counted(self):
        lines = ['SHOT main 4 2', ROW0, 'S:AAA', 'SHOTEND']           # a short row: a log line cut into it
        name, w, h, rgb, rows = shot.last_block(lines)
        self.assertEqual((rows, rgb[:12], rgb[12:]), (2, RGB0, bytes(12)))
        lines = ['SHOT main 4 2', ROW0, 'SHOTEND']                   # a row never came
        self.assertEqual(shot.last_block(lines)[4], 1)

    def test_png_of_the_block(self):
        _, w, h, rgb, _ = shot.last_block(SHOT_LINES)
        path = os.path.join(self.tmp, 'pick.png')
        shot.png(path, w, h, rgb)
        with open(path, 'rb') as f:
            pw, ph, depth, ctype, raw = png_parts(f.read())
        self.assertEqual((pw, ph, depth, ctype), (4, 2, 8, 2))        # 8-bit RGB
        self.assertEqual(raw, b'\0' + RGB0 + b'\0' + RGB1)            # filter 0 before each row
        # the round-panel mask on a 4x4 picture: the corner (0,0) is outside the inscribed circle (tinted red), (1,0)
        # inside (unchanged); the 4x2 block above lies wholly inside a circle of radius 2, so nothing of it is tinted
        masked = png_parts(shot.png_bytes(4, 4, rgb + rgb, round_mask=True))[4]
        self.assertEqual(masked[1:4], bytes([90 + 255 // 2, 0, 0]))
        self.assertEqual(masked[4:7], RGB0[3:6])
        self.assertEqual(png_parts(shot.png_bytes(w, h, rgb, round_mask=True))[4], b'\0' + RGB0 + b'\0' + RGB1)

    def test_distinct_colors(self):
        _, w, h, rgb, _ = shot.last_block(SHOT_LINES)
        self.assertEqual(shot.distinct_colors(rgb, w, h, step=1), 8)
        self.assertEqual(shot.distinct_colors(bytes(24), 4, 2, step=1), 1)   # blank
        self.assertEqual(shot.distinct_colors(rgb, w, h, step=1, limit=3), 3)

    def test_script_writes_a_png_per_block(self):
        log = os.path.join(self.tmp, 'serial_log.txt')
        write(log, '\n'.join(SHOT_LINES + ['I (1) x', 'SHOT main 4 2', ROW1, ROW0, 'SHOTEND']) + '\n')
        out = os.path.join(self.tmp, 'shots')
        with contextlib.redirect_stdout(io.StringIO()) as so:
            n = shot.main([log, out])
        self.assertEqual(n, 2)
        self.assertEqual(sorted(os.listdir(out)), ['main.png', 'pick.png'])
        self.assertIn('4x2', so.getvalue())
        with open(os.path.join(out, 'main.png'), 'rb') as f:
            self.assertEqual(png_parts(f.read())[4], b'\0' + RGB1 + b'\0' + RGB0)
        write(log, 'I (1) nothing here\n')
        with self.assertRaises(SystemExit), contextlib.redirect_stdout(io.StringIO()):
            shot.main([log, out])


class FakeLog:
    """A log whose lines a test appends to (what the monitor would write)."""

    def __init__(self, lines=None):
        self.data = list(lines or [])
        self.pos = self.mark_at = 0

    def lines(self):
        return list(self.data)

    wait = board.Log.wait
    count = board.Log.count
    mark = board.Log.mark
    since_mark = board.Log.since_mark


class SnapOverUsb(unittest.TestCase):
    """Board.snap in no-net mode: `shot <screen> <div>` to the console, SHOTEND waited for, the block decoded."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.b = Board.__new__(Board)
        self.b.no_net = True
        self.b.log = FakeLog(['I (1) diag: mark app ready'])
        self.sent = []

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def stub_send(self, answer):
        def send(line):
            at = len(self.b.log.data)
            self.sent.append(line)
            self.b.log.data += ['> ' + line] + answer
            return at
        self.b.send = send

    def test_snap_returns_the_shot(self):
        self.stub_send(SHOT_LINES[2:])
        out = os.path.join(self.tmp, 'screen_pick.png')
        s = self.b.snap('pick', out)
        self.assertIsInstance(s, Shot)
        self.assertEqual(self.sent, ['shot pick 2'])
        self.assertEqual((s.w, s.h, s.rows, s.path), (4, 2, 2, out))
        self.assertEqual(s.rgb, RGB0 + RGB1)
        self.assertGreaterEqual(s.ms, 0)
        self.assertTrue(os.path.exists(out))
        self.assertEqual(self.b.snap('pick', None, div=4).path, None)
        self.assertEqual(self.sent[-1], 'shot pick 4')

    def test_an_error_line_fails_it(self):
        self.stub_send(['W (5) test: error shot pick: no picture'])
        with self.assertRaises(Fail) as e:
            self.b.snap('pick', None)
        self.assertIn('no picture', str(e.exception))

    def test_no_shotend_times_out(self):
        self.stub_send(['SHOT pick 4 2', ROW0])
        with self.assertRaises(Fail) as e:
            self.b.shot('pick', None, timeout=0.3)
        self.assertIn('SHOTEND', str(e.exception))

    def test_screens_suite_takes_every_screen_over_usb(self):
        # core_suites.every_screen in no-net mode: show, shot (half size: div 2), not blank, snapshot_ms metric
        red, blue = row565(*([0xF800] * 233)), row565(*([0x001F] * 233))
        self.stub_send(['SHOT pick 233 233'] + [red, blue] * 116 + [red] + ['SHOTEND'])   # two colours: not blank
        shown = []
        self.b.show = lambda name: shown.append(name)
        self.b.wait_screen = lambda name, t: None
        ctx = FakeCtx(self.b, self.b.log, self.tmp)
        old = core_suites.CFG['screens'], core_suites.CFG['screens_not_shown']
        core_suites.CFG['screens'], core_suites.CFG['screens_not_shown'] = ['pick'], []
        try:
            core_suites.every_screen(ctx)
        finally:
            core_suites.CFG['screens'], core_suites.CFG['screens_not_shown'] = old
        self.assertIn('pick', shown)
        self.assertIn('snapshot_ms.pick', ctx.metrics)
        self.assertTrue(os.path.exists(os.path.join(self.tmp, 'screen_pick.png')))


# ---------------------------------------------------------------- the boot and console checks on a fake board

class FakeCtx:
    def __init__(self, brd, log, outdir, opts=None):
        self.board, self.log, self.dir = brd, log, outdir
        self.opts = opts or type('O', (), {'ota': None, 'phone': False})()
        self.metrics, self.notes, self.snapped, self.skips = {}, [], set(), {}
        self.reset_ok, self.version = False, ''

    def out(self, name):
        return os.path.join(self.dir, name)

    def metric(self, name, value):
        self.metrics[name] = value

    def skip(self, pattern, reason):
        self.skips[pattern] = reason

    def note(self, text):
        self.notes.append(text)


class FakePager:
    """The pager as the console drives it: `screen NAME` jumps to a page, `swipe left|right` moves one page (the ends
    bounce), `cfg` gives the colour. wait_screen is immediate (the swipe already settled)."""

    def __init__(self, pages=('pick', 'main', 'system'), save_on_return=False):
        self.pages, self.at, self.color, self.save_on_return = list(pages), 0, '#00ff00', save_on_return
        self.log, self.sent, self.seen = FakeLog(), [], []

    def cmd(self, line, expect=None, **kw):
        self.sent.append(line)
        if line.startswith('screen '):
            self.at = self.pages.index(line.split()[1])
        elif line in ('swipe left', 'swipe right'):
            self.at = max(0, min(len(self.pages) - 1, self.at + (1 if line == 'swipe left' else -1)))
            if self.save_on_return and self.pages[self.at] == 'main':
                self.color = '#ff0000'
                self.log.data.append('I (100) cfg: color #ff0000')
        return re.search(r'.*', 'test: ok')

    def show(self, name):
        self.cmd(f'screen {name}')

    def screen(self):
        return self.pages[self.at]

    def wait_screen(self, name, timeout, step=1.0):
        self.seen.append(self.screen())
        if self.screen() != name:
            raise Fail(f'screen is "{self.screen()}", expected "{name}" within {timeout} s')

    def cfg(self):
        return {'color': self.color}


class FakeBoard:
    """Answers console commands from a table: cmd(line, expect) -> the match of `expect` on the reply."""

    def __init__(self, replies, no_net=True):
        self.replies, self.no_net = replies, no_net

    def cmd(self, line, expect=r'test: (ok|pong|screen|heap|wifi|fps|key|where|commands)', timeout=15, **kw):
        if line not in self.replies:
            raise Fail(f"{line}: test: error unknown command '{line}' (help lists them)")
        m = re.search(expect, self.replies[line])
        if not m:
            raise Fail(f'{line}: {self.replies[line]} does not match {expect}')
        return m

    def version(self):
        return self.cmd('ping', r'test: pong (\S+)').group(1)

    def heap(self):
        return {k: int(v) for k, v in kv(self.cmd('heap', r'test: heap (.*)').group(1)).items()}


BOOT = ['ESP-ROM:esp32s3-20210327', 'rst:0x15 (USB_UART_CHIP_RESET),boot:0x8 (SPI_FAST_FLASH_BOOT)',
        'I (420) diag: firmware v0.1.0-rc.0', 'I (421) diag: running from ota_1',
        'I (422) diag: boot reset=USB_UART_CHIP_RESET', 'I (900) diag: mark board      internal 170 KB free',
        'I (2600) test: console ready on USB (send \'help\')', 'I (3100) ble: address aa:bb:cc:dd:ee:ff',
        'I (3200) ble: synced', 'I (3210) ble: advertising 20 ms', 'I (3220) ble: scanning 100/100 ms',
        'I (4895) diag: mark app ready      internal 120 KB free']
HELP = ('test: commands: ping, help, heap, where, memspeed, reboot, fps [reset], tap X Y, press X Y [ms], '
        'swipe left|right|up|down, drag X1 Y1 X2 Y2 [ms], screen [name], color RRGGBB, devmode on|off, name TEXT, '
        'bright 0-100, peers, cfg, hold on|off, shot [SCREEN [DIV]]')
REPLIES = {'ping': 'I (5000) test: pong v0.1.0-rc.0', 'help': 'I (5001) ' + HELP,
           'heap': 'I (5002) test: heap internal=120 min=110 largest=64 psram=7000 psram_min=6900 uptime_s=5 '
                   'failed_allocs=0 lvgl_fallbacks=0',
           'where': 'I (5003) test: where mesh=idle task_lvgl=Blocked'}


class BootCheck(unittest.TestCase):
    def test_boot_image_lines(self):
        self.assertEqual(core_suites.boot_image(BOOT, no_net=True), ('v0.1.0-rc.0', 'ota_1', None))
        self.assertIsNone(core_suites.boot_image(BOOT, no_net=False))      # no ota: Running line: forge_ota absent
        ota = ['I (4952) ota: Running v0.1.1 from ota_0, channel beta']
        self.assertEqual(core_suites.boot_image(ota, no_net=False), ('v0.1.1', 'ota_0', 'beta'))
        self.assertIsNone(core_suites.boot_image(ota, no_net=True))
        self.assertIsNone(core_suites.boot_image([l for l in BOOT if 'running from' not in l], no_net=True))

    def run_start_up(self, lines, version='v0.1.0-rc.0'):
        ctx = FakeCtx(FakeBoard({'ping': f'I (5000) test: pong {version}'}), FakeLog(lines), tempfile.gettempdir())
        core_suites.start_up(ctx)
        return ctx

    def test_start_up_accepts_the_diag_lines(self):
        ctx = self.run_start_up(BOOT)
        self.assertEqual(ctx.metrics['boot_s.ready'], 4.9)
        self.assertTrue(any('v0.1.0-rc.0 from ota_1' in n for n in ctx.notes), ctx.notes)
        self.assertTrue(any('boot stage "app ready"' in n for n in ctx.notes), ctx.notes)

    def test_start_up_wants_the_console_version(self):
        with self.assertRaises(Fail) as e:
            self.run_start_up(BOOT, version='v0.0.9')
        self.assertIn('v0.1.0-rc.0', str(e.exception))
        self.assertIn('v0.0.9', str(e.exception))

    def test_start_up_fails_on_an_error_line(self):
        with self.assertRaises(Fail) as e:
            self.run_start_up(BOOT[:8] + ['E (3150) ble: adv start rc=18'] + BOOT[8:])
        self.assertIn('error lines', str(e.exception))

    def test_start_up_wants_both_diag_lines(self):
        with self.assertRaises(Fail) as e:
            self.run_start_up([l for l in BOOT if 'diag: firmware' not in l])
        self.assertIn('diag: firmware', str(e.exception))

    def test_a_reused_window_is_noted_not_failed(self):
        ctx = self.run_start_up(['I (9000) test: pong x'])       # no rst / ESP-ROM, no ready line: nothing to check
        self.assertIn('boot_s.*', ctx.skips)


class ConsoleExpectation(unittest.TestCase):
    def test_wanted_commands(self):
        want = core_suites.wanted_commands(no_net=True)
        for c in ('ping', 'help', 'heap', 'where', 'reboot', 'color', 'devmode', 'name', 'bright', 'peers', 'cfg',
                  'hold', 'shot'):
            self.assertIn(c, want)
        self.assertNotIn('key', want)
        self.assertNotIn('wifi', want)
        self.assertEqual(core_suites.unwanted_commands(no_net=True), ('key', 'wifi'))
        self.assertIn('wifi', core_suites.wanted_commands(no_net=False))
        self.assertEqual(core_suites.unwanted_commands(no_net=False), ())

    def run_commands(self, help_line):
        ctx = FakeCtx(FakeBoard({**REPLIES, 'help': 'I (5001) ' + help_line}), FakeLog(), tempfile.gettempdir())
        core_suites.commands(ctx)
        return ctx

    def test_help_with_the_app_commands_passes(self):
        ctx = self.run_commands(HELP)
        self.assertTrue(any('console commands' in n for n in ctx.notes))

    def test_a_missing_app_command_fails(self):
        with self.assertRaises(Fail) as e:
            self.run_commands(HELP.replace(', shot [SCREEN [DIV]]', ''))
        self.assertIn('"shot"', str(e.exception))

    def test_network_commands_must_be_gone(self):
        with self.assertRaises(Fail) as e:
            self.run_commands(HELP + ', key, wifi status|offline|online')
        self.assertIn('no network', str(e.exception))

    def test_an_answered_unknown_command_fails(self):
        with self.assertRaises(Fail) as e:
            ctx = FakeCtx(FakeBoard({**REPLIES, 'no-such-command': 'I (1) test: ok'}), FakeLog(), tempfile.gettempdir())
            core_suites.commands(ctx)
        self.assertIn('unknown', str(e.exception))


class AppSuites(unittest.TestCase):
    def test_red_swatch_is_the_top_of_the_ring(self):
        # 12 swatches on a ring of radius 180 around the centre (233, 233), hue 0 = red on top: (233, 53)
        self.assertEqual(app_suites.RED_SWATCH, (CFG['screen']['w'] // 2, CFG['screen']['h'] // 2 - 180))
        self.assertEqual(app_suites.RED_SWATCH, (233, 53))

    def test_put_back_color_only_for_another_valid_colour(self):
        set_to = []
        b = type('B', (), {'color': lambda self, c: set_to.append(c)})()
        app_suites.put_back_color(b, '#ff8000')
        app_suites.put_back_color(b, '#ff0000')                 # red already: nothing to do
        app_suites.put_back_color(b, None)
        app_suites.put_back_color(b, 'garbage')
        self.assertEqual(set_to, ['ff8000'])

    def test_pager_pages(self):
        # one pager, left to right: the colour ring, main (home), system; forge.json names them all
        self.assertEqual(app_suites.PAGES, ['pick', 'main', 'system'])
        self.assertEqual((app_suites.PREV, app_suites.HOME, app_suites.NEXT), ('pick', 'main', 'system'))
        for name in app_suites.PAGES:
            self.assertIn(name, CFG['screens'])
        self.assertEqual([f.__name__ for f in SUITES['navigation']],
                         ['pick_then_main', 'pick_long_press', 'swipe_to_system', 'swipe_to_ring'])
        self.assertIn('color2', [f.__name__ for f in SUITES['mesh']])

    def run_nav(self, fn, brd):
        ctx = FakeCtx(brd, brd.log, tempfile.gettempdir())
        real = app_suites.time.sleep
        app_suites.time.sleep = lambda s: None
        try:
            fn(ctx)
        finally:
            app_suites.time.sleep = real
        return ctx

    def test_swipes_walk_the_pager(self):
        b = FakePager()
        self.run_nav(app_suites.swipe_to_system, b)
        ctx = self.run_nav(app_suites.swipe_to_ring, b)
        self.assertEqual(b.sent, ['screen main', 'swipe left', 'swipe right', 'screen main', 'swipe right', 'swipe left'])
        self.assertEqual(b.seen, ['main', 'system', 'main', 'main', 'pick', 'main'])
        self.assertTrue(any('#00ff00 kept' in n for n in ctx.notes))

    def test_swipe_to_ring_fails_when_the_swipes_save_a_colour(self):
        b = FakePager(save_on_return=True)                 # the old swatch handler: the press of the swipe picked
        with self.assertRaises(Fail) as e:
            self.run_nav(app_suites.swipe_to_ring, b)
        self.assertIn('saved a colour', str(e.exception))

    def test_swipe_to_ring_fails_on_the_old_two_page_pager(self):
        b = FakePager(pages=['main', 'system'])            # before the ring was a page: a swipe right on main bounces
        with self.assertRaises(Fail) as e:
            self.run_nav(app_suites.swipe_to_ring, b)
        self.assertIn('expected "pick"', str(e.exception))

    def test_watch_lines(self):
        hits = lambda line: [what for rx, what in app_suites.APP_WATCH if re.search(rx, line)]
        self.assertTrue(hits('W (100) ble: adv data too long'))
        self.assertTrue(hits('E (100) mesh: queue full'))
        self.assertFalse(hits('I (100) ble: advertising 20 ms'))
        self.assertTrue(hits('I (100) lvgl: draw buf: out of memory'))

    # A swipe from main, as the board logs it with slide.c's drag hook (v0.1.0-rc.6-drag) and before it (rc.6: the
    # safety net released the hold after the slide, ~240 ms of no finger later)
    SWIPE_HOOK = ['I (10000) mesh: hold on', 'I (10021) ui: hold released (drag)', 'I (10021) mesh: hold off',
                  'I (10203) slide: drag: first frame after 12 ms, 20 frames in 180 ms (111 fps), to next',
                  'I (10204) ui: page system']
    SWIPE_OLD = ['I (10000) mesh: hold on',
                 'I (10203) slide: drag: first frame after 12 ms, 20 frames in 180 ms (111 fps), to next',
                 'I (10204) ui: page system',
                 'I (10440) ui: hold released (no finger for 236 ms, or the page changed)', 'I (10440) mesh: hold off']

    def test_swipe_hold_parser(self):
        self.assertEqual(app_suites.swipe_hold(self.SWIPE_HOOK, 'system'), (21, True))
        self.assertEqual(app_suites.swipe_hold(self.SWIPE_OLD, 'system'), (440, False))
        self.assertEqual(app_suites.swipe_hold(['I (5) ui: page pick'], 'pick'), (None, True))       # no hold sent
        self.assertEqual(app_suites.swipe_hold(self.SWIPE_OLD[:3], 'system'), (None, False))      # never ended
        # an earlier "hold off" (a test before) is not this swipe's; "page systemx" is not "page system"
        lines = ['I (1) mesh: hold off'] + self.SWIPE_HOOK[:3] + ['I (9) ui: page systemx']
        self.assertEqual(app_suites.swipe_hold(lines, 'system'), (21, True))
        self.assertLessEqual(21, app_suites.SWIPE_HOLD_MS)
        self.assertGreater(440 - 200, app_suites.SWIPE_HOLD_MS)
        # the slowest right swipe measured on v0.4.0 and v0.7.0 (a full redraw at touch down) passes
        slow = ['I (10000) mesh: hold on', 'I (10109) ui: hold released (drag)', 'I (10109) mesh: hold off',
                'I (10300) ui: page pick']
        gap, before = app_suites.swipe_hold(slow, 'pick')
        self.assertTrue(before)
        self.assertLessEqual(gap, app_suites.SWIPE_HOLD_MS)

    def run_swipes(self, old):
        b = FakePager()
        real = b.cmd

        def cmd(line, expect=None, **kw):
            r = real(line, expect, **kw)
            if line in ('swipe left', 'swipe right'):
                page = b.screen()
                lines = self.SWIPE_OLD if old else self.SWIPE_HOOK
                b.log.data += [re.sub(r'page system', 'page ' + page, l) for l in lines]
            return r
        b.cmd = cmd
        return b, self.run_nav(app_suites.swipe_no_hold, b)

    def test_swipe_no_hold_passes_with_the_drag_hook(self):
        b, ctx = self.run_swipes(old=False)
        self.assertEqual(b.sent, ['screen main', 'swipe left', 'screen main', 'swipe right', 'screen main'])
        self.assertEqual(ctx.notes, ['swipe left -> system: held 21 ms; swipe right -> pick: held 21 ms'])

    def test_swipe_no_hold_fails_on_the_old_firmware(self):
        with self.assertRaises(Fail) as e:
            self.run_swipes(old=True)
        self.assertIn('only after "ui: page system"', str(e.exception))


class FakeColor2(FakePager):
    """The pager plus the wave colour: `color2 RRGGBB|auto` answers like the firmware, `cfg` has "color2", `press X Y
    ms` on a swatch is a long press (new firmware: toggles color2 there, the ring stays) or, with old=True, a tap (the
    firmware before color2: the colour is set and main slides in). legacy=True: a cfg without "color2"."""
    SW = {app_suites.swatch_at(i): '#' + h for i, h in enumerate(
        ['ff0000', 'ff8000', 'ffff00', '80ff00', '00ff00', '00ff80', '00ffff', '0080ff', '0000ff', '8000ff', 'ff00ff',
         'ff0080'])}

    def __init__(self, color2='auto', old=False, legacy=False, answer=None):
        super().__init__()
        self.color2, self.old, self.legacy, self.answer = color2, old, legacy, answer

    def cmd(self, line, expect=None, **kw):
        m = re.fullmatch(r'color2 (auto|[0-9a-fA-F]{6})', line)
        if m:
            self.sent.append(line)
            v = 'auto' if m.group(1) == 'auto' else '#' + m.group(1).lower()
            self.color2 = self.answer or v
            self.log.data.append(f'I (200) cfg: color2 {v}')
            reply = f'I (201) test: ok color2 {self.color2}'
            return re.search(expect or r'.*', reply)
        m = re.fullmatch(r'press (\d+) (\d+) (\d+)', line)
        if m:
            self.sent.append(line)
            hit = self.SW.get((int(m.group(1)), int(m.group(2))))
            if hit and self.old:
                self.color = hit
                self.log.data.append(f'I (300) cfg: color {hit}')
                self.at = self.pages.index('main')
            elif hit:
                self.color2 = 'auto' if self.color2 == hit else hit
                self.log.data.append(f'I (300) cfg: color2 {self.color2}')
            return re.search(r'.*', 'test: ok press')
        return super().cmd(line, expect, **kw)

    def press(self, x=None, y=None, ms=None):
        self.cmd(f'press {x} {y} {ms}')

    def cfg(self):
        c = super().cfg()
        if not self.legacy:
            c['color2'] = self.color2
        return c


class Color2(unittest.TestCase):
    def run_fn(self, fn, b):
        return AppSuites.run_nav(self, fn, b)

    def test_swatch_positions(self):
        # ui.c: x = 233 + lroundf(180 sin a), y = 233 - lroundf(180 cos a), a = i x 30 degrees
        self.assertEqual(app_suites.swatch_at(0), app_suites.RED_SWATCH)
        self.assertEqual(app_suites.GREEN_SWATCH, (389, 323))
        self.assertEqual(app_suites.swatch_at(3), (413, 233))
        self.assertGreaterEqual(app_suites.LONG_PRESS_MS, 400 + 200)    # LVGL's 400 ms with a margin

    def test_console_passes_and_puts_back(self):
        b = FakeColor2(color2='#ff00ff')
        ctx = self.run_fn(app_suites.color2, b)
        self.assertEqual(b.sent, ['color2 00ff00', 'color2 auto', 'color2 ff00ff'])
        self.assertEqual(b.color2, '#ff00ff')
        self.assertIn('#00ff00 then auto', ctx.notes[0])

    def test_console_auto_is_not_put_back_again(self):
        b = FakeColor2(color2='auto')
        self.run_fn(app_suites.color2, b)
        self.assertEqual(b.sent, ['color2 00ff00', 'color2 auto'])

    def test_console_fails_on_a_firmware_without_it(self):
        with self.assertRaises(Fail) as e:
            self.run_fn(app_suites.color2, FakeColor2(legacy=True))
        self.assertIn('no "color2"', str(e.exception))

    def test_console_fails_on_a_wrong_answer(self):
        with self.assertRaises(Fail) as e:
            self.run_fn(app_suites.color2, FakeColor2(answer='#010101'))
        self.assertIn('answered #010101', str(e.exception))

    def test_long_press_passes(self):
        b = FakeColor2()
        ctx = self.run_fn(app_suites.pick_long_press, b)
        self.assertEqual([s for s in b.sent if s.startswith('press')], ['press 389 323 800'] * 2)
        self.assertEqual((b.color2, b.color, b.pages[b.at]), ('auto', '#00ff00', 'pick'))
        self.assertIn('the ring stayed', ctx.notes[0])

    def test_long_press_starts_from_auto_and_puts_back(self):
        b = FakeColor2(color2='#0000ff')
        self.run_fn(app_suites.pick_long_press, b)
        self.assertEqual(b.sent[0], 'color2 auto')
        self.assertEqual(b.sent[-1], 'color2 0000ff')
        self.assertEqual(b.color2, '#0000ff')

    def test_long_press_fails_when_it_is_a_tap(self):
        # the firmware before color2 (or a long press that also counts as a tap): the colour is set, main slides in
        with self.assertRaises(Fail) as e:
            self.run_fn(app_suites.pick_long_press, FakeColor2(old=True))
        self.assertIn('the long press on the green swatch', str(e.exception))


class WaveTap(unittest.TestCase):
    """mesh.wave_tap: a tap at the centre of main -> ui and mesh wave lines within 1 s, the frame line >= 20 fps."""
    END = 'I (14100) display: wave frames 118, fill avg 14.2 max 16.0 ms, frame avg 26.0 max 31.5 ms, 29.4 fps (end)'

    def run_tap(self, after_tap):
        b = FakePager()
        b.at = 1
        real = b.cmd

        def cmd(line, expect=None, **kw):
            r = real(line, expect, **kw)
            if line.startswith('tap '):
                b.log.data += after_tap
            return r
        b.cmd = cmd
        b.tap = lambda x, y: b.cmd(f'tap {x} {y}')
        return b, AppSuites.run_nav(self, app_suites.wave_tap, b)

    def test_parser(self):
        v = app_suites.wave_stats(self.END)
        self.assertEqual((v['frames'], v['fill_avg'], v['frame_max'], v['fps'], v['why']), (118, 14.2, 31.5, 29.4, 'end'))
        self.assertIsNone(app_suites.wave_stats('I (1) display: frame 12.0 + 9.0 ms'))

    def test_passes(self):
        b, ctx = self.run_tap(['I (10010) ui: wave centre at 233,233', 'I (10011) mesh: wave centre from self', self.END])
        self.assertIn('tap 233 233', b.sent)
        self.assertIn('29.4 fps', ctx.notes[0])

    def test_no_wave_from_the_tap_fails(self):
        # the old firmware: a tap on main is only a short hold
        with self.assertRaises(Fail) as e:
            self.run_tap(['I (10000) mesh: hold on', 'I (10120) mesh: hold off'])
        self.assertIn('ui: wave centre', str(e.exception))

    def test_slow_frames_fail(self):
        with self.assertRaises(Fail) as e:
            self.run_tap(['I (10010) ui: wave centre at 233,233', 'I (10011) mesh: wave centre from self',
                          self.END.replace('29.4 fps', '12.5 fps')])
        self.assertIn('12.5 fps', str(e.exception))

    def test_a_paused_wave_fails(self):
        with self.assertRaises(Fail) as e:
            self.run_tap(['I (10010) ui: wave centre at 233,233', 'I (10011) mesh: wave centre from self',
                          self.END.replace('(end)', '(paused)')])
        self.assertIn('paused', str(e.exception))


KEY_A = 'wmg5gDSUg0k8gAEE1eXE09CFB7MxdtkqAgNZgBWyInBOIbaBwgZLKpdqtTu0cmQc5qlNwFMKwLyGn+CojO0dzQ=='   # 88 characters
KEY_B = 'BBBBBBBB' + KEY_A[8:]


class FakeAdmin(FakeLog):
    """The console's secure-update commands as the firmware answers them: admin on|off|key|selftest, trust KEY|none,
    trust (the full key), cfg with admin / admin_key / trusted. legacy: a cfg without them; tamper: the selftest
    accepted a changed message; deaf_trust: "trust" answers ok but cfg never changes; ellipsis: the character after
    the short id in "ok trust" (the helper's ASCII log turns U+2026 into '???')."""

    def __init__(self, admin=False, key=None, trusted=None, legacy=False, tamper=False, deaf_trust=False,
                 ellipsis='\u2026'):
        super().__init__()
        self.admin, self.key, self.trusted = admin, key, trusted
        self.legacy, self.tamper, self.deaf_trust, self.ellipsis = legacy, tamper, deaf_trust, ellipsis
        self.log, self.sent = self, []

    def reply(self, line):
        if line == 'admin on':
            self.key = self.key or KEY_A
            self.admin = True
            return [f'I (1) fw: admin on (key {self.key[:8]})', f'I (1) test: ok admin on {self.key[:8]}']
        if line == 'admin off':
            self.admin = False
            return ['I (1) fw: admin off', 'I (1) test: ok admin off']
        if line == 'admin key':
            return [f'I (1) test: admin key {self.key}'] if self.key else ['W (1) test: error admin key: no key yet']
        if line == 'admin selftest':
            return [f'I (1) test: ok admin selftest 331 sign_ms=113 verify_ms=218 '
                    f'tamper={"ACCEPTED" if self.tamper else "refused"} stack_free=4556 internal_min=123']
        if line == 'trust':
            return [f'I (1) test: trust {self.trusted or "none"}']
        if line == 'trust none':
            self.trusted = None
            return ['I (1) fw: trusted admin none', 'I (1) test: ok trust none']
        if line.startswith('trust '):
            k = line.split()[1]
            if not self.deaf_trust:
                self.trusted = k
            return [f'I (1) fw: trusted admin {k[:8]}', f'I (1) test: ok trust {k[:8]}{self.ellipsis}']
        if line == 'cfg':
            c = {'color': '#ff0000', 'dev': True, 'peers': []}
            if not self.legacy:
                c.update(admin=self.admin, admin_key=self.key if self.admin else None,
                         trusted=self.trusted[:8] if self.trusted else None)
            return ['I (1) test: cfg ' + json.dumps(c)]
        return ['I (1) test: ok']

    def cmd(self, line, expect=None, timeout=15, **kw):
        self.sent.append(line)
        at = len(self.data)
        self.data += ['> ' + line] + self.reply(line)
        return self._match(line, expect, at)

    def _match(self, line, expect, at):
        for l in self.data[at:]:
            if re.search(r'test: error', l):
                raise Fail(f'{line}: {l}')
            m = re.search(expect or r'test: ok', l)
            if m:
                return m
        raise Fail(f'{line}: no answer matching {expect}')

    def cfg(self):
        return json.loads(self.cmd('cfg', r'test: cfg (\{.*\})').group(1))


class Admin(unittest.TestCase):
    """mesh.admin: the console sequence of secure updates, and the board's own pairing put back."""

    def run_admin(self, b):
        return AppSuites.run_nav(self, app_suites.admin, b)

    def test_passes_and_leaves_the_board_as_it_was(self):
        b = FakeAdmin()
        ctx = self.run_admin(b)
        self.assertEqual([x for x in b.sent if x != 'cfg'],
                         ['trust', 'admin on', 'admin key', 'admin selftest', 'trust ' + KEY_A, 'trust none',
                          'admin off', 'trust none', 'admin off'])
        self.assertEqual((b.admin, b.trusted, b.key), (False, None, KEY_A))   # the key pair stays, as "admin off"
        self.assertIn('selftest 331 ms', ctx.notes[0])

    def test_puts_back_admin_on_and_the_trusted_key(self):
        b = FakeAdmin(admin=True, key=KEY_A, trusted=KEY_B)
        self.run_admin(b)
        self.assertEqual(b.sent[-2:], ['trust ' + KEY_B, 'admin on'])
        self.assertEqual((b.admin, b.trusted), (True, KEY_B))

    def test_the_helpers_ascii_ellipsis_passes(self):
        self.run_admin(FakeAdmin(ellipsis='???'))
        self.assertTrue(re.search(app_suites.TRUST_OK, 'test: ok trust wmg5gDSU\u2026'))
        self.assertFalse(re.search(app_suites.TRUST_OK, 'test: ok trust wmg5gD'))

    def test_a_firmware_without_it_fails(self):
        with self.assertRaises(Fail) as e:
            self.run_admin(FakeAdmin(legacy=True))
        self.assertIn('without secure updates', str(e.exception))

    def test_a_selftest_that_accepts_a_changed_message_fails(self):
        b = FakeAdmin(tamper=True)
        with self.assertRaises(Fail) as e:
            self.run_admin(b)
        self.assertIn('ACCEPTED', str(e.exception))
        self.assertEqual((b.admin, b.trusted), (False, None))                 # put back even so

    def test_trust_that_does_not_reach_cfg_fails(self):
        with self.assertRaises(Fail) as e:
            self.run_admin(FakeAdmin(deaf_trust=True))
        self.assertIn('cfg says trusted None', str(e.exception))

    def test_key_pattern(self):
        self.assertEqual(len(KEY_A), 88)
        self.assertTrue(re.fullmatch(app_suites.ADMIN_KEY, 'test: admin key ' + KEY_A))
        self.assertFalse(re.fullmatch(app_suites.ADMIN_KEY, 'test: admin key ' + KEY_A[:-4]))


class LogWaits(unittest.TestCase):
    def test_ready_line_found_after_start_logs_wait(self):
        # "console ready" lost in the first 2.5 s: start_log's wait stops on the ready line itself and moves the read
        # position past it; harness.py's own wait for that line must still find it (start=0), not fail 90 s later
        log = board.Log()
        lines = ['I (4895) diag: mark app ready      internal 126 KB free', 'I (4952) diag: running from ota_0']
        log.lines = lambda: lines
        log.wait(BOOT_SEEN, 1)
        self.assertEqual(log.pos, 1)
        self.assertTrue(log.wait(board.CFG['ready_line'], 1, start=0))
        with self.assertRaises(board.Fail):
            log.wait(board.CFG['ready_line'], 0.3)


class Helpers(unittest.TestCase):
    def test_kv(self):
        self.assertEqual(kv('connected=1 sta_ssid=Home ap=0 ap_pass=abcd1234'),
                         {'connected': '1', 'sta_ssid': 'Home', 'ap': '0', 'ap_pass': 'abcd1234'})

    def test_netsh_language(self):
        self.assertTrue(english_netsh('    Name                   : Wi-Fi\n    State                  : connected\n'))
        self.assertFalse(english_netsh('There is no wireless interface on the system.'))
        with self.assertRaises(Fail):
            english_netsh('    Nom   : Wi-Fi\n    Description : x\n    GUID : y\n    État : connecté\n')

    def test_wait_online_returns_once_connected(self):
        # a board on a network (the twins): perf ran while Wi-Fi rejoined after the Easy Connect page (46 fps once)
        answers = iter(['connected=0 retries=1', 'connected=0 retries=2', 'connected=1 sta_ssid=Home'])
        fake = type('B', (), {'no_net': False, 'wifi': lambda self: kv(next(answers))})()
        self.assertGreaterEqual(board.Board.wait_online(fake, timeout=5, step=0), 0)
        never = type('B', (), {'no_net': False, 'wifi': lambda self: {'connected': '0'}})()
        with self.assertRaises(Fail):
            board.Board.wait_online(never, timeout=0.05, step=0)

    def test_peers_parsing(self):
        # `peers`: n, then one `test: peer` line per peer, read after the reply
        b = Board.__new__(Board)
        b.log = FakeLog()
        b.no_net = True
        lines = ['I (1) test: peers n=2', 'I (2) test: peer aa:bb:cc:dd:ee:01 v0.1.0-rc.0 #0000ff hold rssi=-61',
                 'I (3) test: peer aa:bb:cc:dd:ee:02 v0.1.0-rc.0 #ff0000 idle rssi=-70']
        b.send = lambda line: (b.log.data.extend(['> ' + line] + lines), 0)[1]
        n, peers = b.peers(timeout=0.5)
        self.assertEqual(n, 2)
        self.assertEqual(peers[0], {'addr': 'aa:bb:cc:dd:ee:01', 'ver': 'v0.1.0-rc.0', 'color': '#0000ff',
                                    'holding': True, 'rssi': -61})
        self.assertFalse(peers[1]['holding'])
        b.log.data, lines[:] = [], ['I (1) test: peers n=0']
        self.assertEqual(b.peers(timeout=0.1), (0, []))

    def test_cfg_and_color_replies(self):
        b = Board.__new__(Board)
        b.log = FakeLog()
        b.no_net = True
        reply = {'cfg': 'I (1) test: cfg {"color":"#ff8000","dev":false,"name":"","bright":80,"version":"v0.1.0-rc.0",'
                        '"addr":"aa:bb:cc:dd:ee:ff","peers":[]}',
                 'color ff0000': 'I (2) cfg: color #ff0000', 'hold on': 'I (3) mesh: hold on'}
        b.send = lambda line: (b.log.data.extend(['> ' + line, reply.get(line, ''),
                                                  'I (2) test: ok color #ff0000' if line.startswith('color') else '']),
                               len(b.log.data) - 3)[1]
        self.assertEqual(b.cfg()['color'], '#ff8000')
        self.assertEqual(b.cfg()['peers'], [])
        self.assertEqual(b.color('#ff0000'), '#ff0000')
        at = b.hold(True)
        self.assertTrue(b.log.wait(r'mesh: hold on', 0.2, start=at))

    def test_heap_numbers_settle_after_an_ota_install(self):
        # memory.heap_numbers in an --ota run read 35 KB largest right after the install, 46 at rest
        readings = iter([{'internal': 110, 'largest': 35}, {'internal': 116, 'largest': 41},
                         {'internal': 121, 'largest': 46}, {'internal': 121, 'largest': 46}])
        h = core_suites.heap_settled(lambda: next(readings), settle=True, step=0)
        self.assertEqual(h['largest'], 46)                    # the third reading, confirmed by the fourth
        first = core_suites.heap_settled(lambda: {'internal': 1, 'largest': 2}, settle=False, step=0)
        self.assertEqual(first['largest'], 2)


class ForgeJson(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def write(self, obj, sub=''):
        d = os.path.join(self.tmp, sub)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, 'forge.json'), 'w', encoding='utf-8') as f:
            json.dump(obj, f)

    def test_found_by_walking_up(self):
        self.write({'app': 'demo', 'chip': 'esp32c3', 'screen': {'w': 240}})
        deep = os.path.join(self.tmp, 'tools', 'harness', 'x')
        os.makedirs(deep)
        cfg = forgecfg.load(deep)
        self.assertEqual(os.path.normcase(cfg['_root']), os.path.normcase(self.tmp))
        self.assertEqual((cfg['app'], cfg['chip']), ('demo', 'esp32c3'))
        self.assertEqual(cfg['screen'], {'w': 240, 'h': 466, 'shape': 'rect'})   # defaults fill the rest
        self.assertEqual(cfg['monitor_baud'], 115200)

    def test_nearest_wins(self):
        self.write({'app': 'outer'})
        self.write({'app': 'inner'}, 'sub')
        self.assertEqual(forgecfg.load(os.path.join(self.tmp, 'sub'))['app'], 'inner')

    def test_missing(self):
        with self.assertRaises(FileNotFoundError):
            forgecfg.find_root(self.tmp)                     # (a temp folder has no forge.json above it)

    def test_https_port_default_and_override(self):
        # CONFIG_FORGE_HTTPS_PORT (a co-processor board may not get 443): the tools follow forge.json https_port
        self.write({'app': 'demo'})
        cfg = forgecfg.load(self.tmp)
        self.assertEqual(cfg['https_port'], 443)
        self.assertEqual(forgecfg.https_base(cfg, '10.0.0.5'), 'https://10.0.0.5')
        self.write({'app': 'demo', 'https_port': 8443}, 'p4')
        cfg = forgecfg.load(os.path.join(self.tmp, 'p4'))
        self.assertEqual(forgecfg.https_base(cfg, '10.0.0.5'), 'https://10.0.0.5:8443')

    def test_no_net_default_and_value(self):
        # a project without Wi-Fi says "no_net": true; the twins leave it out and get False (and the Wi-Fi defaults)
        self.write({'app': 'demo'})
        cfg = forgecfg.load(self.tmp)
        self.assertIs(cfg['no_net'], False)
        self.assertEqual(re.search(cfg['ip_line'], 'I (5123) net: Connected, IP 192.168.1.50').group(1), '192.168.1.50')
        self.assertEqual(cfg['setup_ssid'], 'Forge-Setup')
        self.write({'app': 'colormesh', 'no_net': True}, 'nonet')
        self.assertIs(forgecfg.load(os.path.join(self.tmp, 'nonet'))['no_net'], True)

    def test_paths_and_cache(self):
        self.write({'build_dir': 'build/v55'})
        cfg = forgecfg.load(self.tmp)
        self.assertEqual(forgecfg.path(cfg, cfg['build_dir']), os.path.join(self.tmp, 'build', 'v55'))
        self.assertEqual(forgecfg.read_cached(cfg, 'ip'), '')
        forgecfg.write_cached(cfg, 'ip', '10.0.0.7')
        self.assertEqual(forgecfg.read_cached(cfg, 'ip'), '10.0.0.7')
        self.assertTrue(os.path.isdir(os.path.join(self.tmp, '.devloop')))

    def test_the_repository_forge_json(self):
        for k in ('app', 'chip', 'build_dir', 'screens', 'ready_line'):
            self.assertIn(k, CFG)
        self.assertIsInstance(CFG['no_net'], bool)
        self.assertTrue(re.search(CFG['ready_line'], 'I (4000) diag: mark app ready      internal 90 KB free'))
        if CFG['no_net']:                                     # this project: the pick ring and the pager's pages
            for s in ('pick', 'main', 'system'):
                self.assertIn(s, CFG['screens'])


class Stage(unittest.TestCase):
    def test_stage_copies_and_manifest(self):
        sys.path.insert(0, os.path.join(HERE, '..', 'devloop'))
        import stage
        tmp = tempfile.mkdtemp()
        try:
            write(os.path.join(tmp, 'forge.json'), {'app': 'demo', 'build_dir': 'build'})
            b = os.path.join(tmp, 'build')
            os.makedirs(os.path.join(b, 'bootloader'))
            write(os.path.join(b, 'bootloader', 'bootloader.bin'), b'boot')
            write(os.path.join(b, 'demo.bin'), b'app' * 100)
            write(os.path.join(b, 'flasher_args.json'), {'flash_settings': {'flash_mode': 'dio', 'flash_freq': '80m', 'flash_size': '16MB'},
                       'flash_files': {'0x10000': 'demo.bin', '0x0': 'bootloader/bootloader.bin'},
                       'app': {'file': 'demo.bin'}, 'extra_esptool_args': {'chip': 'esp32s3'}})
            m = stage.stage(forgecfg.load(tmp))
            self.assertEqual([p['offset'] for p in m['parts']], ['0x0', '0x10000'])
            d = os.path.join(tmp, '.devloop', 'stage')
            for p in m['parts']:
                self.assertEqual(stage.md5(os.path.join(d, p['file'])), p['md5'])
                self.assertRegex(p['file'], r'^(bootloader|demo)_\d{8}-\d{6}\.bin$')
            self.assertEqual(load_json(os.path.join(d, 'manifest.json'))['chip'], 'esp32s3')
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    unittest.main()

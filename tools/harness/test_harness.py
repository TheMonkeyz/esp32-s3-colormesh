"""Unit tests of the harness's own logic (no board):   python tools/harness/test_harness.py
(or python -m unittest discover -s tools/harness -p "test_*.py")"""
import json
import os
import re
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..'))
import forgecfg  # noqa: E402
from board import CFG, SUITES, english_netsh, kv, Fail  # noqa: E402
import harness  # noqa: E402
from harness import BAD, BASELINE, ORDER, compare, parse_args, propose, suite_of  # noqa: E402

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


class Args(unittest.TestCase):
    def test_default_runs_all_but_ota(self):
        o = parse_args([])
        self.assertNotIn('ota', o.run)
        self.assertIn('boot', o.run)

    def test_quick_leaves_out_the_slow_suites(self):
        o = parse_args(['--quick'])
        self.assertNotIn('idle_stable', o.run)
        self.assertNotIn('wifi_setup', o.run)
        self.assertIn('web', o.run)                          # the web suite's device tests stay; Playwright doesn't
        self.assertEqual(parse_args(['--quick', 'idle_stable']).run, ['idle_stable'])   # asked for: run

    def test_ota_flag_adds_the_suite_and_expect(self):
        o = parse_args(['--ota', 'v1.2.0-rc.1'])
        self.assertIn('ota', o.run)
        self.assertEqual(o.expect, 'v1.2.0-rc.1')

    def test_suite_option_and_flash_default(self):
        o = parse_args(['--suite', 'boot,console', '--flash'])
        self.assertEqual(o.run, ['boot', 'console'])
        self.assertEqual(o.flash, CFG['build_dir'])
        o = parse_args(['--flash', 'boot'])                  # a suite name after --flash is a suite
        self.assertEqual((o.run, o.flash), (['boot'], CFG['build_dir']))

    def test_wifi_setup_only_by_default_on_windows(self):
        # netsh, and a PC that stays online over Ethernet while its Wi-Fi is on the setup network (docs/MACOS.md)
        old = harness.WINDOWS
        try:
            harness.WINDOWS = True
            self.assertIn('wifi_setup', parse_args([]).run)
            harness.WINDOWS = False
            self.assertNotIn('wifi_setup', parse_args([]).run)
            self.assertIn('wifi_runtime', parse_args([]).run)
            self.assertEqual(parse_args(['wifi_setup']).run, ['wifi_setup'])   # still there when asked for
        finally:
            harness.WINDOWS = old


class LogWaits(unittest.TestCase):
    def test_ready_line_found_after_start_logs_wait(self):
        # "console ready" lost in the first 2.5 s: start_log's wait stops on the ready line itself and moves the read
        # position past it; harness.py's own wait for that line must still find it (start=0), not fail 90 s later
        import board
        log = board.Log()
        lines = ['I (4895) diag: mark app ready      internal 126 KB free', 'I (4952) ota: Running v0.1.1 from ota_0']
        log.lines = lambda: lines
        log.wait(r'test: console ready|ota: Running|' + board.CFG['ready_line'], 1)
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
        # perf.page_swipes ran while Wi-Fi rejoined after the Easy Connect page (46 fps once): measure() waits
        import board
        answers = iter(['connected=0 retries=1', 'connected=0 retries=2', 'connected=1 sta_ssid=Home'])
        fake = type('B', (), {'wifi': lambda self: kv(next(answers))})()
        self.assertGreaterEqual(board.Board.wait_online(fake, timeout=5, step=0), 0)
        never = type('B', (), {'wifi': lambda self: {'connected': '0'}})()
        with self.assertRaises(Fail):
            board.Board.wait_online(never, timeout=0.05, step=0)

    def test_measurements_dodge_the_first_minute_update_check(self):
        # perf.page_swipes landed ~60 s after boot in two runs: forge_ota's first check ran inside the measurement
        import app_suites
        self.assertEqual(app_suites.first_minute_wait_s(30), 0)
        self.assertEqual(app_suites.first_minute_wait_s(55), 16)
        self.assertEqual(app_suites.first_minute_wait_s(64), 7)
        self.assertEqual(app_suites.first_minute_wait_s(70), 1)
        self.assertEqual(app_suites.first_minute_wait_s(71), 0)
        self.assertEqual(app_suites.first_minute_wait_s(300), 0)

    def test_heap_numbers_settle_after_an_ota_install(self):
        # memory.heap_numbers in an --ota run read 35 KB largest right after the install, 46 at rest
        import core_suites
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

    def test_paths_and_cache(self):
        self.write({'build_dir': 'build/v55'})
        cfg = forgecfg.load(self.tmp)
        self.assertEqual(forgecfg.path(cfg, cfg['build_dir']), os.path.join(self.tmp, 'build', 'v55'))
        self.assertEqual(forgecfg.read_cached(cfg, 'ip'), '')
        forgecfg.write_cached(cfg, 'ip', '10.0.0.7')
        self.assertEqual(forgecfg.read_cached(cfg, 'ip'), '10.0.0.7')
        self.assertTrue(os.path.isdir(os.path.join(self.tmp, '.devloop')))

    def test_the_repository_forge_json(self):
        for k in ('app', 'chip', 'build_dir', 'screens', 'ready_line', 'ip_line', 'key_env', 'setup_ssid'):
            self.assertIn(k, CFG)
        m = re.search(CFG['ip_line'], 'I (5123) net: Connected, IP 192.168.1.50')
        self.assertEqual(m.group(1), '192.168.1.50')
        self.assertTrue(re.search(CFG['ready_line'], 'I (4000) diag: mark app ready      internal 90 KB free'))


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

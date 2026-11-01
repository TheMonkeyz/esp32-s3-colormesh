#!/usr/bin/env python3
"""Autonomous test harness for an espforge firmware (contract: docs/PROTOCOL.md, project values: forge.json).

    python tools/harness/harness.py                      # every suite on the firmware that is on the board
    python tools/harness/harness.py boot console screens # some suites (also: --suite boot,console)
    python tools/harness/harness.py --flash              # stage and flash the build in forge.json's build_dir first
    python tools/harness/harness.py --flash build/other  # another build folder
    python tools/harness/harness.py wifi_setup --phone   # also the Easy Connect phone step (asks you)
    python tools/harness/harness.py --update-baseline    # propose the measured numbers as the new reference
    python tools/harness/harness.py --ota v1.2.0-rc.1    # install a published release with the display's updater, test it

Generic suites: core_suites.py; the app's own: app_suites.py. Needs the flash helper (Windows: tools/devloop/
start_flash_helper.bat; macOS / Linux: python tools/devloop/flash_helper.py, docs/MACOS.md) and firmware with the
test console. wifi_setup joins the PC's Wi-Fi card to the display's setup network (Ethernet keeps the PC online):
Windows only, left out of the default run elsewhere. Exit code 0 = all passed, no performance problem.
Report: tools/harness/reports/<date-time>/report.md (+ screenshots, results.json, the log of each failed test).
"""
import argparse
import fnmatch
import glob
import json
import os
import re
import subprocess
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from board import CFG, DEV, ROOT, SUITES, Board, Fail, Log, PCWifi  # noqa: E402
import core_suites  # noqa: E402,F401  (registers the generic suites)
# A network name with emoji (a phone shared one over Easy Connect) crashed print() on the Windows console's code page
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding='utf-8', errors='replace')
    except (AttributeError, ValueError):
        pass
import app_suites  # noqa: E402  (registers the app's suites)

ORDER = (['boot', 'console', 'memory', 'screens'] + app_suites.APP_ORDER
         + ['web', 'update', 'idle_stable', 'wifi_runtime', 'wifi_setup', 'ota'])
# wifi_setup joins the computer's Wi-Fi to the display's setup network (netsh, Ethernet keeping the PC online):
# Windows only, left out of the default run elsewhere (a MacBook would lose the display and the internet)
WINDOWS = os.name == 'nt'
HERE = os.path.dirname(os.path.abspath(__file__))
BASELINE = os.path.join(HERE, 'baseline.json')

# Log lines that aren't failures but must not go unseen, for any firmware (the app adds APP_WATCH)
CORE_WATCH = [(r'Task watchdog got triggered', 'task watchdog warnings'),
              (r'test: error .*busy', 'a console command found the display busy')]

# The suite that produces a metric: a baseline metric is only expected (else MISSING) when its suite ran
METRIC_SUITES = [('boot_', 'boot'), ('internal_', 'memory'), ('psram_', 'memory'), ('failed_allocs', 'memory'),
                 ('lvgl_internal_fallbacks', 'memory'), ('idle_', 'idle_stable'), ('snapshot_ms.', 'screens'),
                 ('page_kb', 'web'), ('reconnect_s', 'wifi_runtime')] + app_suites.METRIC_SUITES
BAD = ('REGRESSION', 'MISSING', 'NEW')
HIGHER_IS_WORSE = ('page_kb', 'drop_kb')              # exceptions to "a low _kb or fps number is bad"


class Ctx:
    def __init__(self, board, log, wifi, outdir, opts):
        self.board, self.log, self.wifi, self.dir, self.opts = board, log, wifi, outdir, opts
        self.metrics, self.notes, self.snapped, self.skips = {}, [], set(), {}
        self.reset_ok = False                          # tests that restart the board on purpose set it
        self.version = ''

    def out(self, name):
        return os.path.join(self.dir, name)

    def metric(self, name, value):
        self.metrics[name] = value

    def skip(self, pattern, reason):
        """Metrics matching `pattern` (fnmatch, e.g. 'boot_s.*') were not measured on purpose: the report says why
        instead of failing them as MISSING. Tests must use this, never leave a metric out silently."""
        self.skips[pattern] = reason
        print(f'    (not measured: {pattern}: {reason})', flush=True)

    def note(self, text):
        self.notes.append(text)
        print('    ' + text, flush=True)

    def ask(self, text):
        """A step only a person can do: printed for the agent to relay at once, and left in .devloop/harness.ask."""
        print('\n>>> ASK THE USER: ' + text + '\n', flush=True)
        open(os.path.join(DEV, 'harness.ask'), 'w', encoding='utf-8').write(text)


def elf_path(cfg=CFG):
    """The ELF of the build last staged (stage.py --build build/forge: tools/forge_local.py's), else forge.json's."""
    build = cfg['build_dir']
    try:
        with open(os.path.join(DEV, 'stage', 'manifest.json'), encoding='utf-8') as f:
            build = json.load(f).get('build_dir') or build
    except (OSError, ValueError):
        pass
    return os.path.join(ROOT, *build.split('/'), cfg['app'] + '.elf')


def addr2line(chip=None):
    """The toolchain's addr2line for the chip (Xtensa or RISC-V), from ESP-IDF's tools folder; None if absent."""
    chip = chip or CFG['chip']
    base = os.path.expanduser('~/.espressif/tools')
    pats = [f'{base}/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-{chip}-elf-addr2line*',
            f'{base}/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-addr2line*']
    for pat in pats:
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[-1]
    return None


def crash_summary(lines, version=None, elf=None):
    """The panic reason and the backtrace decoded with the build's ELF, if that ELF is the firmware that crashed
    (its version string is in it); else says so instead of printing wrong function names."""
    why = next((l.strip() for l in lines if re.search(r'assert failed|Guru Meditation|abort\(\) was called|panic', l)), '')
    bt = next((l for l in lines if l.startswith('Backtrace:')), '')
    elf = elf or elf_path()
    head = why[:90] + ' | ' if why else ''
    if bt and version and os.path.exists(elf) and version.encode() not in open(elf, 'rb').read():
        return head + f'backtrace not decoded: {os.path.relpath(elf, ROOT)} is not {version}'
    a2l = addr2line()
    frames = []
    if bt and a2l and os.path.exists(elf):
        addrs = [x.split(':')[0] for x in bt.split()[1:] if x.startswith('0x')]
        out = subprocess.run([a2l, '-pfC', '-e', elf] + addrs, capture_output=True, text=True).stdout
        frames = [l.split(' at ')[0] + ' (' + os.path.basename(l.split(' at ')[-1]) + ')' for l in out.splitlines()
                  if ' at ' in l and 'panic' not in l and 'abort' not in l and 'assert' not in l]
    elif bt:
        return head + bt[:120] + (' (no addr2line found)' if not a2l else ' (no ELF)')
    return head + ' < '.join(frames[:5])


def suite_of(metric, table=None):
    return next((s for pfx, s in (table or METRIC_SUITES) if metric.startswith(pfx)), None)


def compare(metrics, base, ran, skips=None, mode='QIO'):
    """[(metric, value, limit, verdict)] against the baseline {"metric": {"max"|"min": n, "ref": n, "note": ...}}.
    An entry may hold a "dio" set ({"max": n, "ref": n}) used instead on a board whose flash runs in DIO mode (a board
    updated over the air keeps its DIO bootloader: rendering ~30 % slower).
    Verdicts: ok, REGRESSION (outside the limit), MISSING (in the baseline, its suite ran, not measured), NEW
    (measured, no limit yet), or "skipped: why" (ctx.skip). Nothing passes silently: in the source project a third of
    the metrics once had no limit and a reused log window dropped all the boot ones without a word."""
    skips = skips or {}
    why = lambda k: next((r for pt, r in skips.items() if fnmatch.fnmatchcase(k, pt)), None)
    rows = []
    for k in sorted(set(metrics) | {k for k in base if not k.startswith('_') and suite_of(k) in ran}):
        b, v = base.get(k), metrics.get(k)
        if b and mode == 'DIO' and 'dio' in b:
            b = {**b, **b['dio']}
        if v is None:
            rows.append((k, '', '', f'skipped: {why(k)}' if why(k) else 'MISSING'))
        elif not b or not ('max' in b or 'min' in b):
            rows.append((k, v, '', 'NEW'))
        elif 'max' in b:
            rows.append((k, v, f'≤ {b["max"]}', 'ok' if v <= b['max'] else 'REGRESSION'))
        else:
            rows.append((k, v, f'≥ {b["min"]}', 'ok' if v >= b['min'] else 'REGRESSION'))
    return rows


def propose(metrics, base, mode='QIO'):
    """--update-baseline: the baseline with this run's numbers as "ref", limits and notes kept (a placeholder note
    goes: the number is measured now); metrics without an entry get a proposed limit to check by hand (direction
    guessed from the name, marked "proposed")."""
    out = json.loads(json.dumps(base))
    for k, v in metrics.items():
        if not isinstance(v, (int, float)):
            continue
        if k in out and mode == 'DIO' and 'dio' in out[k]:
            out[k]['dio']['ref'] = v                   # a DIO board's numbers go to the DIO set
        elif k in out:
            out[k]['ref'] = v
            if out[k].get('note', '').startswith('placeholder'):
                out[k]['note'] = 'placeholder limit, measured ref: set the limit from it'
        else:
            lo_is_bad = any(s in k for s in ('_kb', 'fps')) and not any(s in k for s in HIGHER_IS_WORSE)
            out[k] = {'ref': v, ('min' if lo_is_bad else 'max'): round(v * (0.75 if lo_is_bad else 1.4), 1),
                      'note': 'proposed by --update-baseline: check the limit and the direction'}
    return out


def find_ip(log):
    """The display's address from its log (forge.json ip_line), the last one; else .devloop/ip; None if neither."""
    rx = re.compile(CFG['ip_line'])
    m = next((m for m in map(rx.search, reversed(log.lines())) if m), None)
    if m:
        return m.group(1)
    try:
        return open(os.path.join(DEV, 'ip')).read().strip() or None
    except OSError:
        return None


def parse_args(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('suites', nargs='*', help='any of: ' + ', '.join(ORDER) + ' (default: all but ota)')
    ap.add_argument('--suite', action='append', default=[], help='suites, comma-separated (same as the positionals)')
    ap.add_argument('--ip', help="the display's address on the home network (default: from the log, forge.json "
                    'ip_line, then .devloop/ip)')
    ap.add_argument('--flash', nargs='?', const=CFG['build_dir'], metavar='BUILD_DIR',
                    help="stage and flash a build first (default: forge.json's build_dir)")
    ap.add_argument('--phone', action='store_true', help='wifi_setup: ask for a phone to test Easy Connect')
    ap.add_argument('--update-baseline', action='store_true',
                    help='write baseline.proposed.json with the measured numbers as the reference')
    ap.add_argument('--minutes', type=int, default=40, help='log window to request from the flash helper')
    ap.add_argument('--quick', action='store_true',
                    help='iterating: leave out idle_stable, wifi_setup and the Playwright suite (a full run before an rc)')
    ap.add_argument('--expect', help='fail unless the board runs this version (e.g. v1.2.0-rc.2)')
    ap.add_argument('--ota', metavar='VERSION', help="install this published release with the display's own updater "
                    'first (waits until its channel offers it), then test it (implies --expect and the ota suite)')
    ap.add_argument('--ota-wait', type=int, default=20, help='--ota: minutes to wait for the channel to offer it')
    opts = ap.parse_args(argv)
    if opts.ota and not opts.expect:
        opts.expect = opts.ota
    if opts.flash in SUITES:                           # "--flash boot": boot is a suite, not a build folder
        opts.suites.insert(0, opts.flash)
        opts.flash = CFG['build_dir']
    asked = opts.suites + [s.strip() for x in opts.suite for s in x.split(',') if s.strip()]
    for s in asked:
        if s not in SUITES:
            ap.error(f'unknown suite {s} (known: {", ".join(x for x in ORDER if x in SUITES)})')
    if 'ota' in asked and not opts.ota:
        ap.error('the ota suite needs --ota VERSION')
    opts.run = asked or [s for s in ORDER if (s not in core_suites.ON_REQUEST or (s == 'ota' and opts.ota))
                         and (WINDOWS or s != 'wifi_setup') and not (opts.quick and s in core_suites.QUICK_SKIP)]
    core_suites.QUICK = opts.quick
    if not asked and not WINDOWS:
        print('(wifi_setup left out: it needs the Windows PC, docs/MACOS.md)', flush=True)
    return opts


def flash_mode(lines):
    """QIO / DIO from the bootloader's or spi_flash's line: render times depend on it (QIO ~30 % faster)."""
    rx = re.compile(r'(?:boot\.\w+: SPI Mode\s*:|spi_flash: flash io:)\s*(\w+)')
    return next((m.group(1).upper() for m in map(rx.search, reversed(lines)) if m), '?')


def main(argv=None):
    opts = parse_args(argv)
    suites = opts.run
    if 'web' in suites:
        core_suites.webtest_start()                 # board-free: runs alongside the board's suites
    outdir = os.path.join(HERE, 'reports', time.strftime('%Y-%m-%d_%H%M%S'))
    os.makedirs(outdir, exist_ok=True)
    log = Log()
    board = Board(opts.ip or find_ip(log) or '', log)
    ctx = Ctx(board, log, PCWifi(), outdir, opts)
    results = []
    started = time.time()
    version, mode = '?', '?'

    reuse = board.helper_status() == 'logging' and not opts.flash
    print(('Using the running log window' if reuse else
           ('Flashing ' + opts.flash if opts.flash else 'Restarting the board') + ' and logging') + ' ...', flush=True)
    try:
        if not reuse:
            board.start_log(opts.minutes * 60, opts.flash and os.path.join(ROOT, opts.flash))
            # From the window's first line: start_log's own wait may have stopped on this very line (it accepts it
            # too, when "console ready" was lost in the first 2.5 s) and moved the read position past it; waiting
            # from there failed 90 s later with the line in the log (v0.1.1-align.1, October 4)
            log.wait(CFG['ready_line'], 90, 'start-up (forge.json ready_line)', start=0)
            time.sleep(3)
        elif not any(re.search(CFG['ready_line'], l) for l in log.lines()):
            # A window that is still at start-up (the helper restarted, or another flash): commands sent now go
            # unanswered until the console is up
            log.wait(CFG['ready_line'], 90, 'start-up (forge.json ready_line)')
            time.sleep(3)
        version = board.version()
        if board.before and version != board.before:
            raise Fail(f'the board ran {board.before} before the restart and {version} after: the bootloader rolled back')
        if opts.ota and version != opts.ota:
            print(f'Waiting for the display\'s channel to offer {opts.ota}, then installing it (running {version})',
                  flush=True)
            board.install(opts.ota, opts.ota_wait)
            time.sleep(5)
            version = board.version()
        if opts.expect and version != opts.expect:
            raise Fail(f'the board runs {version}, not {opts.expect}')
        if not opts.ip:
            board.ip = find_ip(log) or board.ip
        if not board.ip:
            raise Fail('the display\'s address is not in the log yet: pass --ip')
        mode = flash_mode(log.lines())
        print(f'Testing {version} at {board.ip} (flash {mode})', flush=True)
        if mode == 'DIO':
            ctx.note('flash mode DIO: times are held to the baseline\'s "dio" limits where it has them')
    except Fail as e:
        print('Cannot start:', e)
        return 2
    ctx.version = version
    open(os.path.join(DEV, 'ip'), 'w').write(board.ip)

    watch = CORE_WATCH + app_suites.APP_WATCH
    for s in [x for x in ORDER if x in suites] + [x for x in suites if x not in ORDER]:
        for fn in SUITES[s]:
            name = f'{s}.{fn.__name__}'
            print(f'- {name}', flush=True)
            log.mark()                                  # the harness's own cursor: tests keep their own positions
            ctx.reset_ok = False
            t0 = time.time()
            try:
                fn(ctx)
                status, detail = 'pass', ''
            except Fail as e:
                status, detail = 'FAIL', str(e)
            except Exception as e:                      # a bug in the harness itself, or the board vanished
                status, detail = 'ERROR', f'{type(e).__name__}: {e}'
                traceback.print_exc()
            resets = [l for l in log.since_mark() if 'rst:0x' in l]
            if resets and not ctx.reset_ok:             # a crash (also when a check failed after it): panic + backtrace
                status, detail = 'FAIL', (f'unexpected restart: {resets[0][:60]}; {crash_summary(log.since_mark(), version)}'
                                          + (f' (then: {detail})' if detail else ''))
            if status != 'pass' and s in ('wifi_runtime', 'wifi_setup'):   # leave the board and the PC as they were
                for undo in (lambda: board.cmd('wifi online', r'test: wifi (.*)'), ctx.wifi.leave):
                    try:
                        undo()
                    except Exception as e:
                        print(f'  (cleanup: {e})', flush=True)
            for rx, what in watch:
                n = sum(1 for l in log.since_mark() if re.search(rx, l))
                if n:
                    ctx.note(f'{name}: {what} ({n}x)')
            dt = time.time() - t0
            if status != 'pass':
                with open(os.path.join(outdir, f'{name}.log.txt'), 'w', encoding='utf-8') as f:
                    f.write('\n'.join(log.since_mark()))
            print(f'  {status} ({dt:.0f} s) {detail}', flush=True)
            results.append({'test': name, 'status': status, 'seconds': round(dt), 'detail': detail})

    board.stop_log()
    base = json.load(open(BASELINE, encoding='utf-8')) if os.path.exists(BASELINE) else {}
    rows = compare(ctx.metrics, base, suites, ctx.skips, mode)
    if opts.update_baseline:
        prop = os.path.join(HERE, 'baseline.proposed.json')
        json.dump(propose(ctx.metrics, base, mode), open(prop, 'w', encoding='utf-8'), indent=1, sort_keys=True,
                  ensure_ascii=False)
        print(f'Proposed baseline: {os.path.relpath(prop, ROOT)} (review it, then copy it over baseline.json)')
    perf_bad = [r for r in rows if r[3] in BAD]
    failed = [r for r in results if r['status'] != 'pass']
    write_report(outdir, version, results, failed, perf_bad, rows, ctx, time.time() - started)
    print(f'\n{len(results) - len(failed)}/{len(results)} passed, {len(perf_bad)} performance problems')
    for k, v, lim, vd in rows:
        if vd in BAD:
            print(f'  {vd}: {k} {v} {lim}'.replace('≤', '<=').replace('≥', '>='))   # (a cp1252 console)
    print('Report:', os.path.relpath(os.path.join(outdir, 'report.md'), ROOT))
    return 1 if failed or perf_bad else 0


def write_report(outdir, version, results, failed, perf_bad, rows, ctx, seconds):
    lines = [f'# Harness report {time.strftime("%Y-%m-%d %H:%M")}: {CFG["app"]} {version}', '',
             f'{len(results) - len(failed)}/{len(results)} tests passed, {len(perf_bad)} performance problems '
             f'(regression, missing or no limit), {seconds:.0f} s.', '',
             '| test | result | time | detail |', '|---|---|---|---|']
    lines += [f'| {r["test"]} | {r["status"]} | {r["seconds"]} s | {r["detail"]} |' for r in results]
    if ctx.notes:
        lines += ['', '## Notes', ''] + [f'- {n}' for n in ctx.notes]
    if rows:
        lines += ['', '## Performance', '', '| metric | value | limit | |', '|---|---|---|---|']
        lines += [f'| {k} | {v} | {lim} | {"**" + vd + "**" if vd in BAD else vd} |' for k, v, lim, vd in rows]
    shots = sorted(f for f in os.listdir(outdir) if f.endswith('.png'))
    if shots:
        lines += ['', '## Screens', ''] + [f'![{s}]({s})' for s in shots]
    open(os.path.join(outdir, 'report.md'), 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    json.dump({'version': version, 'results': results, 'metrics': ctx.metrics, 'notes': ctx.notes,
               'skips': ctx.skips}, open(os.path.join(outdir, 'results.json'), 'w'), indent=1)


if __name__ == '__main__':
    sys.exit(main())

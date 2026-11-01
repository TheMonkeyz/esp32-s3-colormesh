"""The harness's view of the world: the board (through the flash helper's files in .devloop/ and its test console),
its HTTPS API and snapshots, and the PC's Wi-Fi card (netsh) acting as a phone on the setup network. Also the test
registry the suites use (@test, check, Fail). Contract: docs/PROTOCOL.md; project values: forge.json.

No-net mode (forge.json "no_net": true, or harness.py --no-net; a board without Wi-Fi): nothing uses HTTPS (key, api,
request, install, wait_online raise Fail), and snap() takes the screen over the USB console (`shot`, tools/shot.py).

Standard library only.
"""
import json
import os
import re
import socket
import ssl
import struct
import subprocess
import sys
import time
import http.client
import urllib.request
from typing import NamedTuple

HERE = os.path.dirname(os.path.abspath(__file__))
# tools/ before tools/devloop/: both hold a shot.py, the harness wants tools/shot.py (the devloop one is the one-off
# for a log file by hand)
sys.path.insert(0, os.path.join(HERE, '..', 'devloop'))
sys.path.insert(0, os.path.join(HERE, '..'))
import forgecfg  # noqa: E402  (tools/forgecfg.py)
import shot  # noqa: E402  (tools/shot.py)
try:
    import snapshot  # noqa: E402  (tools/snapshot.py: HTTPS snapshots; a no-net project may not ship it)
except ImportError:
    snapshot = None

CFG = forgecfg.load(HERE)
ROOT = CFG['_root']
DEV = forgecfg.devloop(CFG)
NO_NET = bool(CFG.get('no_net'))                   # no Wi-Fi: console only (harness.py --no-net sets it per run)
SETUP_IP = CFG['setup_subnet'] + '.1'
SCREEN_C = (CFG['screen']['w'] // 2, CFG['screen']['h'] // 2)   # the centre: taps and long-presses
# A line that says the firmware is up after a reset: "console ready" prints at ~2.6 s, often inside the ~2.5 s a PC
# monitor misses after esptool's reset (LESSONS L154); forge_ota's "ota: Running", a no-net firmware's
# "diag: running from ota_N", or the ready line say the same later
BOOT_SEEN = r'test: console ready|ota: Running|diag: running from|' + CFG['ready_line']


class Fail(Exception):
    """A check failed: the message says what was expected and what happened."""


# ---------------------------------------------------------------- test registry (core_suites.py, app_suites.py)

SUITES = {}


def test(suite):
    """@test('name'): add the function(ctx) to a suite; tests run in the order they are defined."""
    def deco(fn):
        SUITES.setdefault(suite, []).append(fn)
        return fn
    return deco


def check(cond, msg):
    if not cond:
        raise Fail(msg)


def p(name):
    """A file in .devloop/."""
    return os.path.join(DEV, name)


class Log:
    """.devloop/serial_live.txt, written line by line by monitor.ps1 while the helper is logging."""

    def __init__(self):
        self.pos = 0          # lines already looked at by wait()
        self.mark_at = 0      # start of the current test (for since_mark)

    def lines(self):
        try:
            with open(p('serial_live.txt'), encoding='utf-8', errors='replace') as f:
                return f.read().splitlines()
        except OSError:
            return []

    def mark(self):
        self.mark_at = self.pos = len(self.lines())

    def since_mark(self):
        return self.lines()[self.mark_at:]

    def wait(self, pattern, timeout, what=None, start=None):
        """First line matching pattern (regex) after the read position (or after line `start`, which leaves the
        read position alone: console replies use that, so an event logged just before a reply is still found by
        the next wait). Returns the re.Match."""
        rx = re.compile(pattern)
        end = time.time() + timeout
        while True:
            lines = self.lines()
            for i in range(self.pos if start is None else start, len(lines)):
                m = rx.search(lines[i])
                if m:
                    if start is None:
                        self.pos = i + 1
                    return m
            if time.time() > end:
                raise Fail(f'no log line /{pattern}/ within {timeout} s' + (f' ({what})' if what else ''))
            time.sleep(0.2)

    def count(self, pattern, start=None):
        """Lines matching pattern since the harness's mark, or since line `start` (tests use their own positions:
        the mark is the harness's, it finds the crashes of the test)."""
        rx = re.compile(pattern)
        return sum(1 for l in (self.since_mark() if start is None else self.lines()[start:]) if rx.search(l))


def kv(line):
    """'a=1 b=x' -> {'a': '1', 'b': 'x'} (console replies)."""
    return dict(x.split('=', 1) for x in line.split() if '=' in x)


class Shot(NamedTuple):
    """A screen taken over the USB console (Board.snap in no-net mode)."""
    ms: int          # from the command sent to SHOTEND in the log
    path: str        # the PNG written (None when no `out` was given)
    w: int           # pixels: the screen's size / div
    h: int
    rows: int        # rows that arrived (h when the block is whole)
    rgb: bytes       # w * h * 3


class Board:
    def __init__(self, ip, log, no_net=None):
        self.ip, self.log = ip, log
        self.no_net = NO_NET if no_net is None else no_net
        self._key = None                                # the display's key (forge_net web), asked once from the console
        self.before = None
        self.ctx = ssl.create_default_context()
        self.ctx.check_hostname = False
        self.ctx.verify_mode = ssl.CERT_NONE          # the board's certificate is self-signed

    def _needs_net(self, what):
        if getattr(self, 'no_net', NO_NET):
            raise Fail(f'{what}: this project has no network (forge.json no_net / --no-net): the board has no HTTPS '
                       'API, use the console')

    # ---- flash helper ----
    def helper_status(self):
        try:
            return open(p('flash.status')).read().strip()
        except OSError:
            return ''

    def version_before_restart(self):
        """Version running now, after waiting for a fresh update to be confirmed. A restart in the first 60 s of an
        update rolls it back to the previous firmware (seen: the harness tested the old build instead of the rc).
        No-net: None (the helper is idle before a restart, so there is no console to ask either)."""
        if self.no_net:
            return None
        try:
            u = self.api('/api/update')
        except Exception:
            return None                                # offline or no API: can't tell, carry on
        if u.get('pending_verify'):
            wait = max(0, 75 - int(u.get('uptime_s', 0)))
            print(f'  {u["current"]} was just installed and is not confirmed yet: waiting {wait} s before restarting',
                  flush=True)
            time.sleep(wait)
            u = self.api('/api/update')
            if u.get('pending_verify'):
                raise Fail(f'{u["current"]} still not confirmed after {u.get("uptime_s")} s: not restarting it')
        return u.get('current')

    def start_log(self, seconds, flash_build=None):
        """Restart the board (or flash the build in `flash_build` first) and log for `seconds`. Waits until logging
        and the test console is up."""
        if self.helper_status() not in ('idle', 'flash_failed', ''):
            raise Fail(f'flash helper busy ({self.helper_status()})')
        self.before = None if flash_build else self.version_before_restart()
        # serial_live.txt goes too: "logging" shows ~2 s before monitor.ps1 recreates it, and the old one would be
        # read as the new boot
        for f in ('flash.done', 'serial_live.txt'):
            if os.path.exists(p(f)):
                os.remove(p(f))
        if flash_build:
            import stage                               # tools/devloop/stage.py: unique names, md5-checked copies
            try:
                print(stage.summary(stage.stage(CFG, flash_build)), flush=True)
            except (OSError, ValueError, KeyError) as e:
                raise Fail(f'staging the build failed: {e}')
            req = 'flash.request'
        else:
            req = 'reboot.request'
        with open(p(req + '.tmp'), 'w') as f:
            f.write(str(seconds))
        os.replace(p(req + '.tmp'), p(req))
        end = time.time() + 240
        while self.helper_status() != 'logging':
            if self.helper_status() == 'flash_failed' and os.path.exists(p('flash.done')):
                raise Fail(f'the flash helper failed: {open(p("flash.done")).read().strip()} (see .devloop/flash_helper.log)')
            if time.time() > end:
                raise Fail('the flash helper did not start logging (is it running? Windows: tools/devloop/'
                           'start_flash_helper.bat; macOS / Linux: python tools/devloop/flash_helper.py, docs/MACOS.md)')
            time.sleep(1)
        self.log.pos = 0
        self.forget()                                  # other firmware now, or the same one restarted: ask again
        # "console ready" prints at ~2.6 s, often inside the ~2.5 s a PC monitor misses after esptool's reset (USB
        # re-enumerates, docs/LESSONS.md L154): the later ota line or the ready line say the same. A restart through
        # the test console keeps the whole boot (L191); a flash, or firmware without the console, still loses it
        self.log.wait(BOOT_SEEN, 30, 'firmware with the test console')

    def stop_log(self):
        if self.helper_status() == 'logging':
            open(p('stop.request'), 'w').close()
            end = time.time() + 30
            while not os.path.exists(p('flash.done')) and time.time() < end:
                time.sleep(0.5)

    def forget(self):
        """Drop what is cached about the board (PROTOCOL.md: reset after every flash or install)."""
        self._key = None
        for f in ('key',):
            try:
                os.remove(p(f))
            except OSError:
                pass

    # ---- test console ----
    # Commands that only read: sent again once when an answer is lost. The USB console has dropped a command line
    # now and then with the board fine (weather_amoled: "wifi status" right after a burst of Wi-Fi log lines, no
    # answer; "where" answered at once 15 s later, October 4). A lost read is not a firmware failure (LESSONS L184).
    READ_ONLY = ('screen', 'wifi status', 'heap', 'ping', 'key', 'help', 'cfg', 'peers')

    def send(self, line):
        """A line to the console through the helper's serial.send, waited for its "> line" echo in the log (the
        monitor polls the file every 200 ms). Returns the log length before the write: waits for what the command
        causes start there. cmd() adds the wait for the reply."""
        at = len(self.log.lines())
        with open(p('serial.send.tmp'), 'w') as f:
            f.write(line + '\n')
        os.replace(p('serial.send.tmp'), p('serial.send'))
        self.log.wait(re.escape('> ' + line), 5, 'helper passed the command on', start=at)
        return at

    def cmd(self, line, expect=r'test: (ok|pong|screen|heap|wifi|fps|key|where|commands)', timeout=15, _again=True):
        at = self.send(line)
        try:
            m = self.log.wait(r'test: error .*|' + expect, timeout, line, start=at)
        except Fail as e:
            if line == 'where':
                raise
            # no answer: the console task is blocked (usually on the display lock). "where" takes no lock.
            try:
                w = self.cmd('where', r'test: where (.*)', timeout=5).group(1)
            except Fail:
                w = 'no answer either'
            if _again and w != 'no answer either' and line in self.READ_ONLY:
                print(f'  (no answer to "{line}" while the console answers "where": sent again)', flush=True)
                return self.cmd(line, expect, timeout, _again=False)
            raise Fail(f'{e}; where: {w}')
        if m.group(0).startswith('test: error'):
            if 'busy' in m.group(0) and line != 'where':
                w = self.cmd('where', r'test: where (.*)', timeout=5).group(1)
                raise Fail(f'{line}: {m.group(0)}; where: {w}')
            raise Fail(f'{line}: {m.group(0)}')
        return re.search(expect, m.string)            # the groups of the expected answer

    def version(self):
        return self.cmd('ping', r'test: pong (\S+)', timeout=10).group(1)

    def screen(self):
        return self.cmd('screen', r'test: screen (\S+)').group(1)

    def show(self, name):
        self.cmd(f'screen {name}', r'test: ok screen (\S+)')

    def wifi(self):
        """connected, sta_ssid, portal, ap, ap_clients, dpp, retries, channel, ap_pass (PROTOCOL.md §2)."""
        return kv(self.cmd('wifi status', r'test: wifi (.*)').group(1))

    def wait_online(self, timeout=60, step=1.0):
        """Wait until the station is back on the home network (`wifi status` connected=1); the seconds it took.
        A setup page takes the radio off the network (Easy Connect), and the first measurement after it landed while
        Wi-Fi rejoined: 46 fps and a 146 ms gap once, a console reply lost once (v0.3.0-rc.1's run, 2026-10-07).
        (A reconnection starts no update check of its own; the one a minute after boot is dodged by measure().)"""
        Board._needs_net(self, 'wait_online')
        t0 = time.time()
        while self.wifi().get('connected') != '1':
            if time.time() - t0 > timeout:
                raise Fail(f'Wi-Fi not back within {timeout} s (wifi status)')
            time.sleep(step)
        return round(time.time() - t0, 1)

    def power(self):
        """cpu_mhz, panel (asleep|awake), screen_lock (0|1): the board's "power" command (PROTOCOL.md §2)."""
        return kv(self.cmd('power', r'test: power (.*)').group(1))

    def heap(self):
        line = self.cmd('heap', r'test: heap (.*)').group(1)
        return {k: int(v) for k, v in kv(line).items() if v.lstrip('-').isdigit()}

    def tap(self, x=None, y=None):
        self.cmd(f'tap {SCREEN_C[0] if x is None else x} {SCREEN_C[1] if y is None else y}')

    def press(self, x=None, y=None, ms=None):
        self.cmd(f'press {SCREEN_C[0] if x is None else x} {SCREEN_C[1] if y is None else y}'
                 + (f' {ms}' if ms else ''))

    # ---- the app's console commands (ColorMesh: color, devmode, cfg, peers, hold; docs/PROTOCOL.md) ----
    def cfg(self):
        """The board's configuration as one JSON line: color, dev, name, bright, version, addr, peers[]."""
        return json.loads(self.cmd('cfg', r'test: cfg (\{.*\})').group(1))

    def color(self, rrggbb):
        """`color RRGGBB` -> the colour set, as '#rrggbb' (the log also says `cfg: color #rrggbb`)."""
        return '#' + self.cmd(f'color {rrggbb.lstrip("#")}', r'test: ok color #([0-9a-fA-F]{6})').group(1).lower()

    def devmode(self, on):
        return self.cmd(f'devmode {"on" if on else "off"}', r'test: ok dev (on|off)').group(1)

    PEER = r'test: peer (\S+) v(\S+) #([0-9a-fA-F]{6}) (hold|idle) rssi=(-?\d+)'

    def peers(self, timeout=2.0):
        """`peers`: (n, [{'addr', 'ver', 'color', 'holding', 'rssi'}]) from `test: peers n=N` and the N `test: peer`
        lines after it (waited up to `timeout` s: the console writes them one by one)."""
        at = len(self.log.lines())
        n = int(self.cmd('peers', r'test: peers n=(\d+)').group(1))
        end = time.time() + timeout
        while True:
            found = [m for m in map(re.compile(self.PEER).search, self.log.lines()[at:]) if m]
            if len(found) >= n or time.time() > end:
                break
            time.sleep(0.2)
        return n, [{'addr': m.group(1), 'ver': 'v' + m.group(2), 'color': '#' + m.group(3).lower(),
                    'holding': m.group(4) == 'hold', 'rssi': int(m.group(5))} for m in found]

    def hold(self, on):
        """`hold on|off`: the simulated finger (the log answers `mesh: hold on|off`; no `test:` reply is promised).
        Returns the log position to wait from."""
        return self.send(f'hold {"on" if on else "off"}')

    def wait_screen(self, name, timeout, step=1.0):
        end = time.time() + timeout
        while True:
            s = self.screen()
            if s == name:
                return
            if time.time() > end:
                raise Fail(f'screen is "{s}", expected "{name}" within {timeout} s')
            time.sleep(step)

    # ---- network API (home network, HTTPS) ----
    def key(self):
        """The display's key (every POST and /api/snapshot need it): the test console's "key" command, saved in
        .devloop/key for snapshot.py. '' when there is no console (no log window) or no answer."""
        Board._needs_net(self, 'key')
        if self._key is None:
            if self.helper_status() != 'logging':      # no console without the helper's log window (yet)
                return forgecfg.read_cached(CFG, 'key')
            try:
                self._key = self.cmd('key', r'test: key ([0-9a-f]{16})', timeout=5).group(1)
                forgecfg.write_cached(CFG, 'key', self._key)
            except Fail:
                self._key = ''
        return self._key

    def headers(self, extra=None):
        h = dict(extra or {})
        if self.key():
            h['X-Key'] = self.key()
        return h

    def api(self, path, data=None, timeout=10):
        Board._needs_net(self, f'api {path}')
        req = urllib.request.Request(forgecfg.https_base(CFG, self.ip) + path, method='POST' if data is not None else 'GET',
                                     data=json.dumps(data).encode() if data is not None else None,
                                     headers=self.headers({'Content-Type': 'application/json'}))
        with urllib.request.urlopen(req, context=self.ctx, timeout=timeout) as r:
            return json.loads(r.read().decode())

    def request(self, method, path, body=None, headers=None, tls=True, host=None):
        """(status, Location header) of one raw request: for the "who may change things" rules."""
        Board._needs_net(self, f'request {method} {path}')
        c = (http.client.HTTPSConnection(self.ip, int(CFG['https_port']), context=self.ctx, timeout=10) if tls
             else http.client.HTTPConnection(self.ip, 80, timeout=10))
        h = {'Host': host or self.ip}
        h.update(headers or {})
        try:
            c.request(method, path, body=body, headers=h)
            r = c.getresponse()
            r.read()
            return r.status, r.getheader('Location')
        finally:
            c.close()

    def install(self, want, wait_min=20):
        """Install `want` with the display's own updater, as a person would: ask it to check its channel until it
        offers `want` (after a tag, CI and the Pages site take ~5 min), install, then wait until the new firmware
        runs and is confirmed (a restart in its first 60 s would roll it back). Needs a log window. The display's
        channel (Beta / Stable) is left as it is: an rc needs Beta."""
        Board._needs_net(self, f'install {want}')
        end = time.time() + wait_min * 60
        while True:
            u = self.api('/api/update', {'action': 'check'})
            for _ in range(40):                        # the check runs in the OTA task: a few seconds
                if u.get('state') != 'checking':
                    break
                time.sleep(1)
                u = self.api('/api/update')
            if u.get('current') == want:
                return
            if u.get('latest') == want and u.get('state') == 'available':
                break
            offer = f'the {u.get("channel")} channel offers {u.get("latest") or "nothing newer"}'
            if time.time() > end:
                raise Fail(f'{offer}, not {want}, after {wait_min} min (CI failed, or {want} needs the Beta channel?)')
            print(f'  {offer} (running {u.get("current")}): waiting for {want}', flush=True)
            time.sleep(60)
        print(f'  offered: installing {want}', flush=True)
        at = len(self.log.lines())
        self.api('/api/update', {'action': 'install'})
        self.forget()                                  # the new firmware: ask its key again
        self.log.wait(r'ota: Update installed, restarting', 300, 'download and install', start=at)
        self.log.wait(r'ota: Running ' + re.escape(want) + ' from', 90, f'{want} starting', start=at)
        self.log.wait(r'ota: New firmware ran \d+ s[^:]*: marked valid', 120, 'the update confirmed (no rollback)', start=at)

    def snap(self, screen, out=None, div=2):
        """(milliseconds, BMP bytes) of GET /api/snapshot; saves a PNG to `out` (masked on a round panel).
        No-net: a Shot (ms, path, w, h, rows, rgb) through the console's `shot <screen> <div>` (div 1..4: every
        div-th pixel, so w = screen width / div), waited for its SHOTEND line (30 s), the PNG written to `out`."""
        if self.no_net:
            return self.shot(screen, out, div)
        if snapshot is None:
            raise Fail('tools/snapshot.py is not in this project: HTTPS snapshots need it')
        t0 = time.time()
        req = urllib.request.Request(forgecfg.https_base(CFG, self.ip) + f'/api/snapshot?screen={screen}', headers=self.headers())
        with urllib.request.urlopen(req, context=self.ctx, timeout=30) as r:
            bmp = r.read()
        ms = (time.time() - t0) * 1000
        if out:
            open(out, 'wb').write(snapshot.bmp_to_png(bmp, snapshot.round_panel(CFG)))
        return ms, bmp

    def shot(self, screen, out=None, div=2, timeout=30):
        """The screen over the USB console (tools/shot.py decodes the SHOT block from the log): a Shot."""
        t0 = time.time()
        at = self.send(f'shot {screen} {div}')
        m = self.log.wait(r'^SHOTEND\s*$|test: error .*|error shot .*', timeout, f'the end of the shot of {screen}',
                          start=at)
        if not m.group(0).startswith('SHOTEND'):
            raise Fail(f'shot {screen} {div}: {m.group(0)}')
        ms = round((time.time() - t0) * 1000)
        blk = shot.last_block(self.log.lines()[at:], screen)
        if not blk:
            raise Fail(f'shot {screen} {div}: SHOTEND arrived but no "SHOT {screen} w h" block decodes from the log')
        name, w, h, rgb, rows = blk
        if out:
            shot.png(out, w, h, rgb, round_mask=CFG['screen'].get('shape') == 'round')
        return Shot(ms, out, w, h, rows, rgb)


class PCWifi:
    """The PC's Wi-Fi card as a phone on the setup network (Ethernet stays up, so the PC stays online). The setup
    network's name starts with forge.json "setup_ssid" (a firmware may add a per-device suffix); its addresses are in
    "setup_subnet"."""

    PROFILE = '''<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
  <name>{ssid}</name>
  <SSIDConfig><SSID><name>{ssid}</name></SSID></SSIDConfig>
  <connectionType>ESS</connectionType><connectionMode>manual</connectionMode>
  <MSM><security>{security}</security></MSM>
</WLANProfile>'''
    WPA2 = ('<authEncryption><authentication>WPA2PSK</authentication><encryption>AES</encryption>'
            '<useOneX>false</useOneX></authEncryption><sharedKey><keyType>passPhrase</keyType>'
            '<protected>false</protected><keyMaterial>{key}</keyMaterial></sharedKey>')
    OPEN = ('<authEncryption><authentication>open</authentication><encryption>none</encryption>'
            '<useOneX>false</useOneX></authEncryption>')

    def __init__(self, iface='Wi-Fi', prefix=None, subnet=None):
        self.iface = iface
        self.prefix = prefix or CFG['setup_ssid']
        self.subnet = subnet or CFG['setup_subnet']
        self.ssid = None                              # the setup network joined

    def netsh(self, *args):
        r = subprocess.run(['netsh', 'wlan', *args], capture_output=True, text=True, errors='replace')
        return r.stdout + r.stderr

    def available(self):
        """True when the PC has a Wi-Fi card. netsh answers in the Windows display language and this class reads its
        English words (Name, State, SSID, Channel, connected): in another language it stops with a clear message
        rather than misread it. False off Windows: netsh only exists there, and a MacBook has no Ethernet to stay
        online while its Wi-Fi joins the setup network (docs/MACOS.md)."""
        if os.name != 'nt':
            return False
        return english_netsh(self.netsh('show', 'interfaces'))

    def scan(self):
        """{ssid: 2.4 GHz channel} of the networks the PC sees (ESP32 radios are 2.4 GHz; a dual-band router lists
        the same name on 5 GHz too)."""
        out = self.netsh('show', 'networks', 'mode=bssid')
        nets, ssid = {}, None
        for line in out.splitlines():
            m = re.match(r'\s*SSID \d+ : (.*)', line)
            if m:
                ssid = m.group(1).strip()
                continue
            m = re.match(r'\s*Channel\s*:\s*(\d+)', line)
            if m and ssid is not None and int(m.group(1)) <= 14 and ssid not in nets:
                nets[ssid] = int(m.group(1))
        return nets

    def find_setup(self, timeout=30):
        """The setup network's name as the PC sees it, else the configured name (None if not seen). Windows' list
        ("netsh wlan show networks") is its last scan, refreshed on its own schedule: a network up for 30 s can be
        missing from it (espforge, October 4). Joining by name doesn't need it, so callers go on with the name."""
        end = time.time() + timeout
        while True:
            names = [s for s in self.scan() if s.startswith(self.prefix)]
            if names:
                return sorted(names)[0]
            if time.time() > end:
                return None
            time.sleep(3)

    def state(self):
        out = self.netsh('show', 'interfaces')
        st = re.search(r'^\s*State\s*:\s*(\S+)', out, re.M)
        ss = re.search(r'^\s*SSID\s*:\s*(.+)$', out, re.M)
        return (st.group(1) if st else '?', ss.group(1).strip() if ss else '')

    def join_setup(self, password, ssid=None, timeout=40):
        """Join the setup network (WPA2 with `password`, or open when it is empty)."""
        self.ssid = ssid or self.find_setup(10) or self.prefix   # by name: a directed connect needs no scan
        sec = self.WPA2.format(key=password) if password else self.OPEN
        path = p('setup_profile.xml')
        open(path, 'w').write(self.PROFILE.format(ssid=self.ssid, security=sec))
        self.netsh('add', 'profile', f'filename={path}', f'interface={self.iface}', 'user=current')
        os.remove(path)                               # it holds the password
        end = time.time() + timeout
        while time.time() < end:
            self.netsh('connect', f'name={self.ssid}', f'interface={self.iface}')
            for _ in range(10):
                time.sleep(1)
                st, now = self.state()
                if st == 'connected' and now == self.ssid and self.has_setup_ip():
                    return
        raise Fail(f'the PC could not join {self.ssid} within {timeout} s (state {self.state()})')

    def has_setup_ip(self):
        out = subprocess.run(['ipconfig'], capture_output=True, text=True, errors='replace').stdout
        return (self.subnet + '.') in out

    def leave(self):
        if os.name != 'nt':
            return
        self.netsh('disconnect', f'interface={self.iface}')
        if self.ssid:
            self.netsh('delete', 'profile', f'name={self.ssid}', f'interface={self.iface}')


def english_netsh(out):
    """`netsh wlan show interfaces` output: True if it lists an interface in English, False if there is no Wi-Fi
    card; raises Fail if it lists one in another language (lines "key : value" but no "Name")."""
    if re.search(r'^\s*Name\s*:', out, re.M):
        return True
    if len(re.findall(r'^\s*\S[^:\n]*?\s+:\s', out, re.M)) >= 3:
        raise Fail("netsh answers in another language than English (the Windows display language): the Wi-Fi tests "
                   "read its English words. Run them on an English Windows, or teach PCWifi the words")
    return False


def dns_query(server, name, timeout=3):
    """A record for name from server (one UDP query); None if no answer."""
    q = struct.pack('>HHHHHH', 0x1234, 0x0100, 1, 0, 0, 0)
    q += b''.join(bytes([len(x)]) + x.encode() for x in name.split('.')) + b'\0' + struct.pack('>HH', 1, 1)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(q, (server, 53))
        r, _ = s.recvfrom(512)
    except OSError:
        return None
    finally:
        s.close()
    if struct.unpack('>H', r[6:8])[0] < 1:
        return None
    return '.'.join(str(b) for b in r[-4:])          # the server answers with one A record at the end


def http_get(host_ip, path, host_header, timeout=5):
    c = http.client.HTTPConnection(host_ip, 80, timeout=timeout)
    try:
        c.request('GET', path, headers={'Host': host_header})
        r = c.getresponse()
        return r.status, dict(r.getheaders()), r.read()
    finally:
        c.close()

#!/usr/bin/env python3
"""Flash helper for macOS and Linux (and Windows): the file protocol of flash_helper.ps1 + monitor.ps1 (docs/PROTOCOL.md
§1), so devloop.py, the harness and snapshot.py work unchanged. Run it from a shell where ESP-IDF's export.sh was
sourced (its Python has esptool and pyserial). See docs/MACOS.md.

    python tools/devloop/flash_helper.py              # helper: waits for flash.request / reboot.request (Ctrl-C)
    python tools/devloop/flash_helper.py stage        # stage.py: forge.json's build_dir into .devloop/stage (md5)
    python tools/devloop/flash_helper.py flash 60     # flash the staged parts once, log 60 s
    python tools/devloop/flash_helper.py reboot 60    # restart once (no flash), log 60 s
    python tools/devloop/flash_helper.py monitor 60   # log only
    python tools/devloop/flash_helper.py ports        # the serial ports, the board's marked

Its files are in .devloop/ (forgecfg), as the PowerShell helper's: flash.request / reboot.request (content: seconds of
log), flash.status, flash.running, flash.done, serial_live.txt, serial_log.txt, serial.send, stop.request,
flash_log.txt, flash_helper.log. What to flash: <build_dir>/flasher_args.json, from the copies stage.py made, each
checked by md5 against stage/manifest.json and against the build (L1).

The board's port: --port, else $FORGE_PORT, else forge.json's port, else the ESP32-S3's own USB (VID 0x303A; macOS
/dev/cu.usbmodem…), else esptool's choice. The board's USB goes away at every hard reset (native USB): the monitor
reopens it, and finds the board again by vendor id if it comes back under another name.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
sys.path.insert(0, HERE)
import forgecfg  # noqa: E402

CFG = forgecfg.load(HERE)
DEV = forgecfg.devloop(CFG)
ESPRESSIF_VID = 0x303A
SILABS_VID = 0x10C4                      # a board with a CP210x bridge (the Guition ESP32-P4)
BOOTED = (b'ESP-ROM:', b'rst:0x')                 # the first lines of a boot (ROM banner, reset reason)


def p(name):
    return os.path.join(DEV, name)


def now():
    return time.strftime('%Y-%m-%dT%H:%M:%S')


def say(msg, quiet=False):
    if not quiet:
        print(f'[{time.strftime("%H:%M:%S")}] {msg}', flush=True)
    with open(p('flash_helper.log'), 'a', encoding='utf-8') as f:
        f.write(f'[{now()}] {msg}\n')


def write(name, text):
    with open(p(name), 'w', encoding='utf-8') as f:
        f.write(text)


def remove(name):
    try:
        os.remove(p(name))
    except OSError:
        pass


def need_serial():
    try:
        import serial  # noqa: F401
        import serial.tools.list_ports  # noqa: F401
    except ImportError:
        sys.exit('pyserial is missing: run this from a shell where ESP-IDF is set up '
                 '(". $IDF_PATH/export.sh" on macOS / Linux), see docs/MACOS.md')


def md5(path):
    h = hashlib.md5()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 16), b''):
            h.update(block)
    return h.hexdigest()


# ---------------------------------------------------------------------------------------------------- ports

def board_ports():
    """[(device, description)] of the ports with Espressif's or Silicon Labs' USB vendor id, best first (macOS: cu.*,
    not tty.*)."""
    import serial.tools.list_ports
    found = [(x.device, x.description) for x in serial.tools.list_ports.comports()
             if x.vid in (ESPRESSIF_VID, SILABS_VID)]
    return sorted(found, key=lambda d: ('/tty.' in d[0], d[0]))


def probe_chip(device, chip):
    """esptool's chip_id on that port: true when the chip there is `chip` (it answers; another chip refuses)."""
    try:
        r = subprocess.run([sys.executable, '-m', 'esptool', '--chip', chip, '-p', device, '--before', 'default_reset',
                            '--after', 'hard_reset', 'chip_id'], capture_output=True, timeout=30)
        return r.returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def pick_port(ports, chip, probe, cached=None):
    """The board's port among `ports` [(device, description)]: the cached one if still connected, else the only one,
    else the first whose chip answers probe(device, chip) (several boards on the PC: an ESP32-P4 beside the S3, as
    MeteoBus's P4 helper did). None when none answers. Pure: test_flash_helper.py."""
    devs = [d for d, _ in ports]
    if cached and cached in devs:
        return cached
    if len(devs) == 1:
        return devs[0]
    for d in devs:
        if probe(d, chip):
            return d
    return None


def find_port(explicit=None):
    if explicit:
        return explicit
    if os.environ.get('FORGE_PORT'):
        return os.environ['FORGE_PORT']
    if CFG.get('port'):
        return CFG['port']
    p = pick_port(board_ports(), CFG.get('chip', 'esp32s3'), probe_chip, forgecfg.read_cached(CFG, 'port'))
    if p:
        forgecfg.write_cached(CFG, 'port', p)         # (.devloop/port: asked again only when it is gone)
    return p


def list_ports():
    import serial.tools.list_ports
    for x in serial.tools.list_ports.comports():
        mark = '  <- ESP32-S3 (the board)' if x.vid == ESPRESSIF_VID else ''
        vid = f'{x.vid:04X}:{x.pid:04X}' if x.vid is not None else '-'
        print(f'{x.device}  [{vid}] {x.description}{mark}')


# ---------------------------------------------------------------------------------------------------- esptool

def esptool_cmd():
    """tools/esptool.exe (the standalone v4.8.1, git-ignored) if present, as flash_helper.ps1; else this Python's."""
    exe = forgecfg.path(CFG, 'tools', 'esptool.exe')
    return [exe] if os.name == 'nt' and os.path.exists(exe) else [sys.executable, '-m', 'esptool']


def esptool(args):
    """Runs esptool; returns (exit code, output lines, the port it used)."""
    lines = []
    try:
        pr = subprocess.Popen(esptool_cmd() + args, cwd=DEV, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, errors='replace', bufsize=1)
    except OSError as e:
        return 1, [f'could not start esptool: {e}'], None
    for line in pr.stdout:
        line = line.rstrip('\r\n')
        lines.append(line)
        if re.search(r'Serial port|Chip is|Writing at .*\(100 ?%\)|Wrote |Hash of data|Hard resetting|[Ee]rror|failed|'
                     r'No module named', line):
            print('    ' + line, flush=True)
    rc = pr.wait()
    write('flash_log.txt', '\n'.join(lines) + '\n')
    port = next((m.group(1) for m in map(re.compile(r'Serial port (\S+)').search, reversed(lines)) if m), None)
    return rc, lines, port


def flash_args(port):
    """(esptool arguments, None), or (None, why not): the parts in <build_dir>/flasher_args.json, from the staged
    copies, each checked by md5 against stage/manifest.json and against the build (a build newer than the stage would
    test old firmware). The build is the one staged (stage.py --build: another folder than forge.json's; checked
    against forge.json's, its copies were refused as "changed since it was staged", 2026-10-09). As Flash-Args in
    flash_helper.ps1."""
    mf_path = p(os.path.join('stage', 'manifest.json'))
    if not os.path.exists(mf_path):
        return None, 'no stage/manifest.json (run tools/devloop/stage.py)'
    with open(mf_path, encoding='utf-8') as f:
        man = json.load(f)
    build = forgecfg.path(CFG, man.get('build_dir') or CFG['build_dir'])
    fa_path = os.path.join(build, 'flasher_args.json')
    if not os.path.exists(fa_path):
        return None, f'no {fa_path} (build the firmware first)'
    with open(fa_path, encoding='utf-8') as f:
        fa = json.load(f)
    fs, extra = fa.get('flash_settings', {}), fa.get('extra_esptool_args', {})
    args = (['--port', port] if port else []) + [
        '--chip', extra.get('chip') or CFG['chip'], '-b', str(CFG['baud']),
        '--before', extra.get('before') or 'default_reset', '--after', extra.get('after') or 'hard_reset']
    if extra.get('stub') is False:
        args.append('--no-stub')
    args += ['write_flash', '--flash_mode', fs['flash_mode'], '--flash_freq', fs['flash_freq'],
             '--flash_size', fs['flash_size']]
    for off, rel in sorted(fa['flash_files'].items(), key=lambda kv: int(kv[0], 16)):
        part = next((x for x in man.get('parts', []) if int(x['offset'], 16) == int(off, 16)), None)
        if not part:
            return None, f'{rel} at {off} is not staged (layout changed since staging? run stage.py again)'
        staged = p(os.path.join('stage', part['file']))
        if not os.path.exists(staged):
            return None, f'staged file {part["file"]} is missing'
        got = md5(staged)
        if got != part['md5'].lower():
            return None, f'staged {part["file"]}: md5 {got}, the manifest says {part["md5"]}'
        src = os.path.join(build, rel)
        if os.path.exists(src) and md5(src) != got:
            return None, f'{rel} changed since it was staged (run stage.py again)'
        say(f'  {off:>9}  {part["file"]}  {os.path.getsize(staged):,} B  md5 ok')
        args += [off, staged]
    return args, None


# ---------------------------------------------------------------------------------------------------- monitor

class Keys:
    """q / Esc in the helper's own terminal ends the log window (as in the PowerShell window). No-op without a tty."""

    def __init__(self):
        self.old = None
        self.win = os.name == 'nt'
        if not self.win and sys.stdin.isatty():
            try:
                import termios
                import tty
                self.old = termios.tcgetattr(sys.stdin)
                tty.setcbreak(sys.stdin.fileno())
            except Exception:
                self.old = None

    def pressed_stop(self):
        try:
            if self.win:
                import msvcrt
                while msvcrt.kbhit():
                    if msvcrt.getwch() in ('q', 'Q', '\x1b'):
                        return True
                return False
            if self.old is None:
                return False
            import select
            while select.select([sys.stdin], [], [], 0)[0]:
                if sys.stdin.read(1) in ('q', 'Q', '\x1b'):
                    return True
        except Exception:
            pass
        return False

    def close(self):
        if self.old is not None:
            import termios
            termios.tcsetattr(sys.stdin, termios.TCSADRAIN, self.old)


def open_port(port, url=False, wait=10.0):
    """The port opened with DTR and RTS low (on the ESP32-S3's USB they reset the chip / select download mode).
    Retries for `wait` s: after a reset the USB device re-enumerates, and may come back under another name (macOS
    numbers usbmodem ports by USB location): then the board is found again by its vendor id. None if it never opens."""
    import serial
    baud = int(CFG['monitor_baud'])
    end = time.time() + wait
    while True:
        try:
            if url:
                s = serial.serial_for_url(port, baudrate=baud, timeout=0.1)
            else:
                s = serial.Serial()
                s.port, s.baudrate, s.timeout = port, baud, 0.1
                s.dtr, s.rts = False, False
                s.open()
            return s
        except (OSError, serial.SerialException):
            if time.time() > end:
                return None
            time.sleep(0.5)
            if not url and not os.path.exists(port):    # (COM names never "exist": the lookup finds the same one)
                ports = board_ports()
                if ports and ports[0][0] != port:
                    say(f'(the board came back as {ports[0][0]}, was {port})', quiet=True)
                    port = ports[0][0]


def console_reboot(port, wait=4.0):
    """Restart the board with the test console's `reboot` (forge_core/testcon), keeping the port open: a software
    restart leaves the S3's USB connected, so the boot log is kept from its first line. esptool's hard reset
    re-enumerates the USB, and the ~2.5 s before the port is back are lost (the flash mode, the reset reason: the
    harness printed "flash ?"; LESSONS L154, L191). Returns (open port, bytes read since the command), or None when no
    boot follows (firmware without the console, a hung board): then use esptool."""
    import serial
    s = open_port(port, wait=3)
    if s is None:
        return None
    buf, end = b'', time.time() + wait
    try:
        s.write(b'reboot\n')
        while time.time() < end:
            buf += s.read(4096)
            if any(m in buf for m in BOOTED):
                return s, buf
    except (OSError, serial.SerialException):
        pass                                        # the USB went away after all: not a console restart
    try:
        s.close()
    except Exception:
        pass
    return None


def monitor(port, seconds, url=False, echo=True, opened=None):
    """Log the board for `seconds` into serial_live.txt (line by line) and serial_log.txt (at the end); pass each line
    of serial.send to the board. `opened`: (port already open, bytes already read), from console_reboot().
    Returns (lines, stopped_early, error)."""
    import serial
    remove('serial.send')                           # stale commands from an earlier run
    remove('serial.send.tmp')
    out = []
    live = open(p('serial_live.txt'), 'w', encoding='utf-8', newline='\n')
    keys = Keys()

    def emit(line):
        out.append(line)
        live.write(line + '\n')
        live.flush()
        if echo:
            print(line, flush=True)

    if opened:
        s, buf = opened
    else:
        if not url:
            time.sleep(2)                           # let USB re-enumerate after the reset
        s, buf = open_port(port, url), b''
    if s is None:
        live.close()
        keys.close()
        write('serial_log.txt', f'Could not open {port}\n')
        return [], False, f'could not open {port}'
    end, stopped, next_check, reopened = time.time() + seconds, False, 0.0, 0
    *done, buf = buf.split(b'\n')                   # what console_reboot() already read
    for raw in done:
        emit(raw.decode('utf-8', errors='replace').rstrip('\r'))
    try:
        while time.time() < end:
            try:
                chunk = s.read(4096)
            except (OSError, serial.SerialException):
                # the board restarted (its USB went away): reopen, keep logging
                try:
                    s.close()
                except Exception:
                    pass
                s = open_port(port, url, wait=max(0.0, min(15.0, end - time.time())))
                if s is None:
                    emit(f'[flash_helper] {port} gone and not back')
                    break
                reopened += 1
                continue
            if chunk:
                buf += chunk
                *done, buf = buf.split(b'\n')
                for raw in done:
                    emit(raw.decode('utf-8', errors='replace').rstrip('\r'))
            if time.time() >= next_check:
                next_check = time.time() + 0.2
                if os.path.exists(p('stop.request')):
                    remove('stop.request')
                    stopped = True
                    break
                if os.path.exists(p('serial.send')):
                    try:
                        with open(p('serial.send'), encoding='utf-8') as f:
                            cmds = f.read().splitlines()
                        os.remove(p('serial.send'))
                    except OSError:
                        cmds = []                   # still being written: next check
                    for c in cmds:
                        if c.strip():
                            s.write((c.strip() + '\n').encode())
                            emit('> ' + c.strip())
                if keys.pressed_stop():
                    stopped = True
                    break
    finally:
        if buf:
            emit(buf.decode('utf-8', errors='replace').rstrip('\r'))
        try:
            s.close()
        except Exception:
            pass
        live.close()
        keys.close()
        write('serial_log.txt', '\n'.join(out) + '\n')
    if reopened:
        say(f'(the port went away and came back {reopened}x: the board restarted)', quiet=not echo)
    return out, stopped, None


# ---------------------------------------------------------------------------------------------------- one request

def run_request(kind, seconds, port_opt=None, echo=True):
    """kind 'flash' or 'reboot': flash (or only restart), then log. Writes flash.status / flash.done as
    flash_helper.ps1 does. Returns the exit code."""
    start = time.time()
    started = now()
    remove('flash.done')
    remove('stop.request')
    write('flash.running', started)
    say(f'=== {"Reboot request, no flashing" if kind == "reboot" else "Flash request"} (serial log: {seconds} s) ===')
    write('flash.status', 'flashing\n')
    port = find_port(port_opt)
    opened, step = None, 'flash'
    if kind == 'reboot' and port:
        opened = console_reboot(port)
    if opened:
        rc, lines, used = 0, [], port
        say('Restarted through the test console (the port stays open: the boot log is kept from its first line)')
    elif kind == 'flash':
        args, err = flash_args(port)
        if err:
            write('flash_log.txt', err + '\n')      # never leave the previous run's log for this failure
            rc, lines, used, step = 3, [err], None, 'verify'
        else:
            rc, lines, used = esptool(args)
    else:
        rc, lines, used = esptool((['--port', port] if port else []) +
                                  ['--chip', CFG['chip'], '--before', 'default_reset', '--after', 'hard_reset',
                                   'chip_id'])
    port = used or port
    flash_s = int(time.time() - start)
    if rc != 0:
        say(f'{"NOT FLASHING" if step == "verify" else kind.upper() + " FAILED"} (exit {rc}) after {flash_s} s '
            '- last lines:')
        for line in lines[-6:]:
            print('    ' + line, flush=True)
        if step == 'flash':
            say('Tip: no port found? Check the cable carries data, run "flash_helper.py ports"; else hold BOOT, '
                'tap RESET, release BOOT, and request again.')
        write('flash.status', 'flash_failed\n')
        write('flash.done', f'exit={rc} port={port or ""} stage={step} started={started} finished={now()}\n')
        remove('flash.running')
        print('\a', end='', flush=True)
        return rc
    say(f'{kind.upper()} OK on {port} in {flash_s} s - board is restarting')
    if kind == 'flash':
        remove('ip')                                # what the tools cached belongs to the old firmware (L7)
        remove('key')
    write('flash.status', 'logging\n')
    say(f'Logging serial output for {seconds} s (saved to .devloop/serial_log.txt). Press q or Esc to stop early ...')
    log, early, err = monitor(port, seconds, echo=echo, opened=opened)
    if early:
        say('Serial log stopped early')
    errs = sum(1 for line in log if line.startswith('E ('))
    warns = sum(1 for line in log if line.startswith('W ('))
    # "resets" are the unexpected ones: not the restart asked for (an esptool reset's own boot is lost, L154)
    resets = max(0, sum(1 for line in log if 'rst:0x' in line) - (1 if opened else 0))
    say(f'Serial log saved: {len(log)} lines, {errs} errors, {warns} warnings, {resets} resets' +
        (f' ({err})' if err else ''))
    total = int(time.time() - start)
    write('flash.done', f'exit={1 if err else 0} port={port} flash_s={flash_s} total_s={total} errors={errs} '
                        f'warnings={warns} resets={resets} stopped_early={int(early)} started={started} '
                        f'finished={now()}\n')
    remove('flash.running')
    write('flash.status', 'idle\n')
    say(f'=== Done in {total} s ===')
    print('\a', end='', flush=True)
    return 1 if err else 0


def seconds_in(name, default=60):
    try:
        with open(p(name), encoding='utf-8', errors='replace') as f:
            m = re.fullmatch(r'\s*(\d{1,5})\s*', f.read())
        return int(m.group(1)) if m else default
    except OSError:
        return default


def helper(port_opt=None, poll=1.0, echo=True):
    """Wait for requests forever (Ctrl-C ends it)."""
    say(f'Flash helper for {CFG["_root"]} (chip {CFG["chip"]}, {CFG["baud"]} baud, build {CFG["build_dir"]})')
    say(f'esptool: {" ".join(esptool_cmd())}')
    port = find_port(port_opt)
    say(f'Board port: {port}' if port else 'No ESP32-S3 port found yet (plug the board in; esptool will look again)')
    # Requests left from before this start: whoever wrote them has given up waiting (the harness waits ~4 min), and a
    # flash nobody watches is a surprise (L157)
    for old in ('flash.request', 'reboot.request', 'stop.request', 'serial.send'):
        if os.path.exists(p(old)):
            remove(old)
            say(f'Ignored a {old} left from before this start')
    write('flash.status', 'idle\n')
    say('Waiting for .devloop/flash.request / reboot.request ...')
    try:
        while True:
            req = next((r for r in ('flash.request', 'reboot.request') if os.path.exists(p(r))), None)
            if not req:
                time.sleep(poll)
                continue
            secs = seconds_in(req)
            remove(req)
            run_request('reboot' if req == 'reboot.request' else 'flash', secs, port_opt, echo)
            say('Waiting for the next request ...')
    except KeyboardInterrupt:
        remove('flash.running')
        write('flash.status', 'stopped\n')
        say('Flash helper stopped')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('action', nargs='?', default='helper',
                    choices=['helper', 'flash', 'reboot', 'monitor', 'stage', 'ports'])
    ap.add_argument('arg', nargs='?', help="seconds of log (flash, reboot, monitor) or the build folder "
                    "(stage; default: forge.json's build_dir)")
    ap.add_argument('--port', help="serial port (default: $FORGE_PORT, forge.json's port, else the ESP32-S3's USB)")
    o = ap.parse_args(argv)
    if o.action == 'stage':
        import stage
        try:
            print(stage.summary(stage.stage(CFG, o.arg)))
        except (OSError, ValueError, KeyError) as e:
            sys.exit(f'stage: {e}')
        return 0
    need_serial()
    if o.action == 'ports':
        list_ports()
        return 0
    if o.action == 'helper':
        helper(o.port)
        return 0
    secs = int(o.arg or 60)
    if o.action == 'monitor':
        port = find_port(o.port)
        if not port:
            sys.exit('no ESP32-S3 port found (flash_helper.py ports)')
        _, early, err = monitor(port, secs)
        if err:
            print(err)
            return 1
        return 2 if early else 0
    return run_request(o.action, secs, o.port)


if __name__ == '__main__':
    sys.exit(main())

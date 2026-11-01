#!/usr/bin/env python3
"""Drive the flash helper (start_flash_helper.bat on Windows, flash_helper.py on macOS / Linux) without hand-writing
its files in .devloop/ (docs/PROTOCOL.md §1). Standard library only.

    python tools/devloop/devloop.py flash [seconds]      # stage the build (stage.py), then flash it and log
    python tools/devloop/devloop.py flash 300 --no-stage # flash what is already staged
    python tools/devloop/devloop.py reboot [seconds]     # restart (no flash; test console, else esptool), then log
    python tools/devloop/devloop.py send "heap"          # a line to the test console (while logging)
    python tools/devloop/devloop.py stop                 # end the log window now
    python tools/devloop/devloop.py status               # helper state, flash.done, the last log lines
    python tools/devloop/devloop.py wait-done [--timeout 600]   # wait for flash.done, print it (exit 1 if it failed)

The serial log: .devloop/serial_live.txt (grows while logging), .devloop/serial_log.txt (the whole window).
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
sys.path.insert(0, HERE)
import forgecfg  # noqa: E402

CFG = forgecfg.load(HERE)
DEV = forgecfg.devloop(CFG)


def f(name):
    return os.path.join(DEV, name)


def read(name):
    try:
        with open(f(name), encoding='utf-8', errors='replace') as fh:
            return fh.read().strip()
    except OSError:
        return ''


def write_atomic(name, text):
    with open(f(name + '.tmp'), 'w', encoding='utf-8') as fh:
        fh.write(text)
    os.replace(f(name + '.tmp'), f(name))


def helper_check():
    st = read('flash.status')
    if not st:
        print('note: no .devloop/flash.status: is the flash helper running (Windows, in the background: powershell '
              '-File tools/devloop/flash_helper.ps1, or start_flash_helper.bat; macOS / Linux: python '
              'tools/devloop/flash_helper.py)?')
    elif st not in ('idle', 'flash_failed'):
        sys.exit(f'the flash helper is busy ({st}): wait, or "devloop.py stop" to end its log window')


def request(kind, seconds):
    helper_check()
    for name in ('flash.done', 'serial_live.txt'):    # an old serial_live.txt would read as the new boot
        try:
            os.remove(f(name))
        except OSError:
            pass
    write_atomic(kind + '.request', str(seconds))
    print(f'{kind}.request written ({seconds} s of log). Follow .devloop/serial_live.txt; "devloop.py wait-done" '
          'waits for the end.')


def cmd_flash(a):
    if not a.no_stage:
        import stage
        try:
            print(stage.summary(stage.stage(CFG, a.build)))
        except (OSError, ValueError, KeyError) as e:
            sys.exit(f'stage: {e}')
    request('flash', a.seconds)


def cmd_reboot(a):
    request('reboot', a.seconds)


def cmd_stop(a):
    if read('flash.status') != 'logging':
        print(f'not logging (status: {read("flash.status") or "unknown"})')
    open(f('stop.request'), 'w').close()


def cmd_send(a):
    if read('flash.status') != 'logging':
        sys.exit('the helper is not logging: commands only reach the board during a log window')
    write_atomic('serial.send', a.line.rstrip('\n') + '\n')
    print(f'sent: {a.line}')


def cmd_status(a):
    print(f'status: {read("flash.status") or "unknown (helper not started?)"}')
    print(f'running: {"yes since " + read("flash.running") if os.path.exists(f("flash.running")) else "no"}')
    if read('flash.done'):
        print(f'flash.done: {read("flash.done")}')
    live = read('serial_live.txt').splitlines()
    if live:
        print(f'serial_live.txt: {len(live)} lines, last:')
        for line in live[-a.lines:]:
            print('  ' + line)


def cmd_wait_done(a):
    end = time.time() + a.timeout
    while not read('flash.done'):
        if time.time() > end:
            sys.exit(f'no flash.done after {a.timeout} s (status: {read("flash.status")})')
        time.sleep(0.5)
    done = read('flash.done')
    print(done)
    sys.exit(0 if done.startswith('exit=0') else 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('flash', help='stage the build, flash it, log')
    p.add_argument('seconds', nargs='?', type=int, default=60)
    p.add_argument('--no-stage', action='store_true', help='flash the parts already in .devloop/stage')
    p.add_argument('--build', help="build folder (default: forge.json's build_dir)")
    p.set_defaults(fn=cmd_flash)
    p = sub.add_parser('reboot', help='restart, log')
    p.add_argument('seconds', nargs='?', type=int, default=60)
    p.set_defaults(fn=cmd_reboot)
    sub.add_parser('stop', help='end the log window').set_defaults(fn=cmd_stop)
    p = sub.add_parser('send', help='a line for the test console')
    p.add_argument('line')
    p.set_defaults(fn=cmd_send)
    p = sub.add_parser('status', help='helper state and the last log lines')
    p.add_argument('--lines', type=int, default=10)
    p.set_defaults(fn=cmd_status)
    p = sub.add_parser('wait-done', help='wait for flash.done')
    p.add_argument('--timeout', type=int, default=900)
    p.set_defaults(fn=cmd_wait_done)
    a = ap.parse_args()
    a.fn(a)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Add up an LVGL profiler trace from the log (console command "profile", docs/PROTOCOL.md §2; profiler builds only).

    python tools/harness/profile.py [.devloop/serial_live.txt]

For each screen between "PROFILE-BEGIN <name>" and "PROFILE-END <name>": total frame time, and per marker
(function or draw task type "t_fill", "t_label"...) its count, inclusive time and self time (minus nested markers).
"""
import os
import re
import sys
from collections import defaultdict

LINE = re.compile(r'LVGL-\d+ \[\d+\] (\d+)\.(\d+): tracing_mark_write: ([BE])\|1\|(.+)')


def report(name, events):
    incl, self_t, cnt = defaultdict(float), defaultdict(float), defaultdict(int)
    stack, first, last = [], None, None
    for t, kind, tag in events:
        first = t if first is None else first
        last = t
        if kind == 'B':
            stack.append([tag, t, 0.0])
        else:
            if not stack:
                continue
            tag0, t0, child = stack.pop()
            d = t - t0
            incl[tag0] += d
            self_t[tag0] += d - child
            cnt[tag0] += 1
            if stack:
                stack[-1][2] += d
    total = (last - first) if first is not None else 0
    print(f'\n## {name}: {total:.1f} ms profiled\n')
    print(f'| marker | count | inclusive ms | self ms | self % |\n|---|---|---|---|---|')
    for tag in sorted(self_t, key=lambda k: -self_t[k])[:18]:
        print(f'| {tag} | {cnt[tag]} | {incl[tag]:.1f} | {self_t[tag]:.1f} | {100 * self_t[tag] / total if total else 0:.0f} |')


def main(path):
    cur, events = None, []
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('PROFILE-BEGIN'):
            cur, events = line.split(None, 1)[1].strip(), []
        elif line.startswith('PROFILE-END') and cur:
            report(cur, events)
            cur = None
        elif cur:
            m = LINE.search(line)
            if m:
                t = int(m.group(1)) * 1000 + int(m.group(2)) / 1000      # ms
                events.append((t, m.group(3), m.group(4)))


if __name__ == '__main__':
    if len(sys.argv) > 1:
        main(sys.argv[1])
    else:
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
        import forgecfg
        main(os.path.join(forgecfg.devloop(forgecfg.load()), 'serial_live.txt'))

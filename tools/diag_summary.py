#!/usr/bin/env python3
"""Summarise the "diag:" lines of a serial log (default: .devloop/serial_log.txt). Line formats: docs/PROTOCOL.md §3
and components/forge_core/diag.c.

    python tools/diag_summary.py [serial_log.txt]
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import forgecfg  # noqa: E402

if len(sys.argv) > 1:
    path = sys.argv[1]
else:
    path = os.path.join(forgecfg.devloop(forgecfg.load()), "serial_log.txt")
lines = open(path, encoding="utf-8", errors="replace").read().splitlines()


def ts(l):
    m = re.match(r"[IWE] \((\d+)\)", l)
    return int(m.group(1)) / 1000 if m else None


marks, bench, boots, coredumps = [], [], [], []
heap, disp = [], []
tasks = {}          # name -> [max cpu %, min stack free B]
cpu = []
errors = [l for l in lines if l.startswith("E (")]
warns = [l for l in lines if l.startswith("W (")]
crashes = [l for l in lines if re.search(r"Guru Meditation|abort\(\)|stack overflow|rst:0x", l)]

for l in lines:
    if "diag: mark" in l:
        # "diag: mark <stage>  internal N KB free (largest N), DMA N KB, PSRAM N KB"; a bare "diag: mark <stage>" too
        m = re.search(r"diag: mark (.+?)\s+internal (\d+) KB free \(largest (\d+)\), DMA (\d+) KB, PSRAM (\d+)", l)
        if m:
            marks.append((f"{ts(l) or 0:.1f}",) + m.groups())
        else:
            m = re.search(r"diag: mark (.+)$", l)
            if m:
                marks.append((f"{ts(l) or 0:.1f}", m.group(1).strip(), "", "", "", ""))
    elif "diag: boot " in l:
        boots.append(l.split("diag: ", 1)[1])               # "boot reset=<reason> flash_mb=N psram_kb=N"
    elif "diag: coredump" in l:
        coredumps.append(l.split("diag: ", 1)[1])           # "coredump: last crash in task ..."
    elif "diag: bench" in l:
        bench.append(l.split("diag: ", 1)[1])
    elif "diag: heap:" in l:
        n = list(map(int, re.findall(r"(\d+)", l.split("heap:", 1)[1])))
        # internal free, min ever, largest now, worst | DMA free, largest now, worst | PSRAM free, min ever, largest
        # | failed allocs, LVGL in internal RAM
        if len(n) >= 10:
            heap.append(n)
    elif "diag: display:" in l:
        m = re.search(r"(\d+) frames, render avg ([\d.]+) ms max ([\d.]+) ms.*animation ([\d.]+) fps \((\d+) frames, "
                      r"worst gap (\d+) ms\) \| LVGL lock wait max ([\d.]+) ms, longest hold ([\d.]+) ms by (\S+)", l)
        if m:
            disp.append(m.groups())
    elif "diag: tasks:" in l:
        for t in l.split("tasks:", 1)[1].split("|"):
            m = re.search(r"(\S.*?)\(c(-?\d) p(\d+)\) ([\d.]+)% (\d+)B", t)
            if not m:
                continue
            name = m.group(1).strip()
            c, s = float(m.group(4)), int(m.group(5))
            old = tasks.get(name, [0.0, 1 << 30])
            tasks[name] = [max(old[0], c), min(old[1], s)]
    elif "diag: cpu:" in l:
        m = re.search(r"core0 (\d+)% busy, core1 (\d+)% busy", l)
        if m:
            cpu.append((ts(l), int(m.group(1)), int(m.group(2))))

print(f"# Diagnostics summary: {path} ({len(lines)} lines)\n")
print(f"Errors {len(errors)}, warnings {len(warns)}, crashes/resets {len(crashes)}")
for l in crashes[:5]:
    print("  !", l[:160])
for b in boots[-3:]:
    print("- " + b)
for c in coredumps:
    print("  ! " + c)
if marks:
    print("\n## Boot (internal RAM free after each stage)\n")
    print("| s | stage | internal KB | largest block KB | DMA KB | PSRAM KB |\n|---|---|---|---|---|---|")
    for m in marks:
        print("| " + " | ".join(m) + " |")
if heap:
    print("\n## Heap over the run\n")
    print(f"- internal free: {min(h[0] for h in heap)}-{max(h[0] for h in heap)} KB, "
          f"min ever {min(h[1] for h in heap)} KB, worst largest block {min(h[3] for h in heap)} KB")
    print(f"- DMA-capable free: min {min(h[4] for h in heap)} KB, worst largest block {min(h[6] for h in heap)} KB")
    print(f"- PSRAM free: {min(h[7] for h in heap)}-{max(h[7] for h in heap)} KB, min ever {min(h[8] for h in heap)} KB")
    if len(heap[-1]) >= 12:
        print(f"- failed allocations {heap[-1][10]}, LVGL blocks in internal RAM {heap[-1][11]} (at the last line)")
if bench:
    print("\n## Render bench\n")
    for b in bench:
        print("- " + b)
if disp:
    live = [d for d in disp if d[8] != "bench"]           # windows without the bench
    anim = [d for d in live if int(d[4]) > 0]
    print("\n## Display (live use, bench windows excluded)\n")
    if live:
        print(f"- worst frame render {max(float(d[2]) for d in live):.1f} ms")
        print(f"- longest wait of LVGL for the lock {max(float(d[6]) for d in live):.1f} ms; longest hold by another task "
              + ", ".join(sorted({f'{d[8]} {float(d[7]):.0f} ms' for d in live if float(d[7]) > 20})))
    if anim:
        fr = sum(int(d[4]) for d in anim)
        print(f"- animation: {fr} back-to-back frames, {min(float(d[3]) for d in anim):.1f}-"
              f"{max(float(d[3]) for d in anim):.1f} fps per window, worst gap {max(int(d[5]) for d in anim)} ms")
if tasks:
    print("\n## Tasks (max CPU % of one core in a window, min free stack)\n")
    print("| task | max CPU % | min free stack B |\n|---|---|---|")
    for name, (c, s) in sorted(tasks.items(), key=lambda kv: -kv[1][0]):
        flag = "  <- low" if s < 1024 and not name.startswith(("ipc", "sys_evt")) else ""
        print(f"| {name} | {c:.1f} | {s}{flag} |")
if cpu:
    print(f"\nCore load per window: core0 max {max(c[1] for c in cpu)}%, core1 max {max(c[2] for c in cpu)}%; "
          f"idle-ish median core0 {sorted(c[1] for c in cpu)[len(cpu)//2]}%")

#!/usr/bin/env python3
"""Screens from a serial log: each "SHOT name w h" ... "SHOTEND" block (the display's `shot` console command: base64
RGB565 rows, every DIV-th pixel) becomes <out>/<name>.png.

    python tools/devloop/shot.py .devloop/serial_log.txt [out_dir]      (default out_dir: shots)

For a session that can't reach the board's HTTPS (tools/snapshot.py): devloop.py send "shot hello 2", then this on
the log. The harness's console suite uses blocks() and png() too. Standard library only. From MeteoBus's tools/shot.py.
"""
import base64
import os
import struct
import sys
import zlib


def png(path, w, h, rgb):
    """rgb: w * h * 3 bytes, rows top to bottom."""
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    raw = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def blocks(lines):
    """(name, w, h, rgb, rows_received) for each complete SHOT block in the lines. A row that doesn't decode to w x 2
    bytes (a log line interleaved with it) is black."""
    cur = None
    for line in lines:
        line = line.strip().lstrip('\ufeff')
        if line.startswith('SHOT ') and not line.startswith('SHOTEND'):
            parts = line.split()
            if len(parts) >= 4 and parts[2].isdigit() and parts[3].isdigit():
                cur = [parts[1], int(parts[2]), int(parts[3]), []]
        elif cur and line.startswith('S:'):
            try:
                cur[3].append(base64.b64decode(line[2:]))
            except Exception:
                cur[3].append(b'')
        elif cur and line.startswith('SHOTEND'):
            name, w, h, rows = cur
            rgb = bytearray()
            for y in range(h):
                r = rows[y] if y < len(rows) and len(rows[y]) == w * 2 else bytes(w * 2)
                for x in range(w):
                    v = r[2 * x] | r[2 * x + 1] << 8
                    rgb += bytes(((v >> 11 & 31) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31))
            yield name, w, h, bytes(rgb), len(rows)
            cur = None


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    log, out = sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else 'shots'
    os.makedirs(out, exist_ok=True)
    n = 0
    with open(log, encoding='utf-8', errors='replace') as f:
        for name, w, h, rgb, rows in blocks(f):
            p = os.path.join(out, name + '.png')
            png(p, w, h, rgb)
            print(p, f'{w}x{h}', f'{rows} rows' + ('' if rows == h else f' (expected {h})'))
            n += 1
    if not n:
        sys.exit('no SHOT block in the log (send "shot <screen> [DIV]" to the console first)')


if __name__ == '__main__':
    main()

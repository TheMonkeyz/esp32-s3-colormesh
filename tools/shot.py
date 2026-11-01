#!/usr/bin/env python3
"""Screens from a serial log: each "SHOT name w h" ... "SHOTEND" block (the display's `shot [screen] [div]` console
command) becomes <out_dir>/<name>.png.

    python tools/shot.py .devloop/serial_log.txt [out_dir]        (default out_dir: shots)

The firmware writes the block raw on the USB console: `SHOT <name> <w> <h>`, then one `S:<base64>` line per row
(w RGB565 pixels, little-endian: every div-th pixel of every div-th row, div 1..4, default 2, so w = screen width /
div), then `SHOTEND`. A row that does not decode to w * 2 bytes (a log line interleaved with it) is black.

Importable (standard library only): the harness's Board.snap() uses last_block() and png() in no-net mode (forge.json
"no_net": no HTTPS on the board), distinct_colors() tells a blank screen. From MeteoBus's tools/shot.py.
"""
import base64
import os
import struct
import sys
import zlib


def rgb565(v):
    """(r, g, b) 0..255 of one RGB565 value."""
    return (v >> 11 & 31) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31


def blocks(lines):
    """(name, w, h, rgb, rows_received) for each complete SHOT block in `lines` (strings, any iterable). rgb is
    w * h * 3 bytes, rows top to bottom; a row that is not w * 2 bytes long is black, but still counted."""
    cur = None
    for line in lines:
        line = line.strip().lstrip('﻿')
        if line.startswith('SHOT ') and not line.startswith('SHOTEND'):
            parts = line.split()
            if len(parts) >= 4 and parts[2].isdigit() and parts[3].isdigit():
                cur = [parts[1], int(parts[2]), int(parts[3]), []]
        elif cur and line.startswith('S:'):
            try:
                cur[3].append(base64.b64decode(line[2:], validate=True))
            except Exception:
                cur[3].append(b'')
        elif cur and line.startswith('SHOTEND'):
            name, w, h, rows = cur
            rgb = bytearray()
            for y in range(h):
                r = rows[y] if y < len(rows) and len(rows[y]) == w * 2 else bytes(w * 2)
                for x in range(w):
                    rgb += bytes(rgb565(r[2 * x] | r[2 * x + 1] << 8))
            yield name, w, h, bytes(rgb), len(rows)
            cur = None


def last_block(lines, name=None):
    """The last complete block (with that name, when given), else None."""
    found = None
    for blk in blocks(lines):
        if name is None or blk[0] == name:
            found = blk
    return found


def png_bytes(w, h, rgb, round_mask=False):
    """A PNG of w * h RGB bytes. round_mask tints the pixels outside the inscribed circle red (a round panel): what
    the circle would cut off stands out, as tools/snapshot.py does."""
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    raw = bytearray()
    for y in range(h):
        line = bytearray(rgb[y * w * 3:(y + 1) * w * 3])
        if round_mask:
            r2, cy = (w / 2) ** 2, y + 0.5 - h / 2
            for x in range(w):
                if (x + 0.5 - w / 2) ** 2 + cy * cy > r2:
                    line[3 * x] = 90 + line[3 * x] // 2
        raw += b'\0' + line
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))


def png(path, w, h, rgb, round_mask=False):
    with open(path, 'wb') as f:
        f.write(png_bytes(w, h, rgb, round_mask))


def distinct_colors(rgb, w, h, step=7, limit=64):
    """How many colours a sample of the picture holds (up to `limit`): 1 means a blank screen."""
    seen = set()
    for y in range(0, h, step):
        for x in range(0, w, step):
            i = (y * w + x) * 3
            seen.add(bytes(rgb[i:i + 3]))
            if len(seen) >= limit:
                return limit
    return len(seen)


def write_blocks(log_path, out_dir):
    """Every block of the log file as <out_dir>/<name>.png: [(png_path, name, w, h, rows_received)]."""
    os.makedirs(out_dir, exist_ok=True)
    out = []
    with open(log_path, encoding='utf-8', errors='replace') as f:
        for name, w, h, rgb, rows in blocks(f):
            p = os.path.join(out_dir, name + '.png')
            png(p, w, h, rgb)
            out.append((p, name, w, h, rows))
    return out


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if not argv:
        sys.exit(__doc__)
    written = write_blocks(argv[0], argv[1] if len(argv) > 1 else 'shots')
    for p, name, w, h, rows in written:
        print(p, f'{w}x{h}', f'{rows} rows' + ('' if rows == h else f' (expected {h})'))
    if not written:
        sys.exit('no SHOT block in the log (send "shot <screen> [DIV]" to the console first)')
    return len(written)


if __name__ == '__main__':
    main()

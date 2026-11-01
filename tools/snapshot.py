#!/usr/bin/env python3
"""Save a screen of the display as a PNG, rendered by the device itself (GET /api/snapshot, docs/PROTOCOL.md §4).

    python tools/snapshot.py hello                  -> snapshot_hello.png
    python tools/snapshot.py system out.png --ip 192.168.1.50

Screens: the names in forge.json "screens" (the test console's "screen" command lists the current one). The device
renders the screen off-display, so the board isn't disturbed. On a round panel (forge.json screen.shape) the pixels
outside the circle are tinted red, so anything the circle would cut off stands out (--square to skip). Needs the PC
on the same network; the certificate is self-signed. Standard library only.

The display's address: --ip, else .devloop/ip (the harness writes it). Its key: --key, else the environment variable
named by forge.json "key_env", else .devloop/key (the harness asks the test console's "key" command and saves it).
"""
import argparse
import os
import ssl
import struct
import sys
import urllib.error
import urllib.request
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import forgecfg  # noqa: E402


def bmp_pixels(bmp: bytes):
    """(w, h, rows): rows top to bottom as RGB bytes."""
    off, = struct.unpack_from("<I", bmp, 10)
    w, h = struct.unpack_from("<ii", bmp, 18)
    bpp, = struct.unpack_from("<H", bmp, 28)
    if bpp != 24:
        raise ValueError(f"expected a 24-bit BMP, got {bpp}")
    top_down = h < 0
    h = abs(h)
    row = (w * 3 + 3) & ~3
    rows = []
    for y in range(h):
        src = off + (y if top_down else h - 1 - y) * row
        line = bytearray(bmp[src:src + w * 3])
        line[0::3], line[2::3] = line[2::3], line[0::3]          # BGR -> RGB
        rows.append(line)
    return w, h, rows


def distinct_colors(bmp: bytes, step: int = 7, limit: int = 64) -> int:
    """How many colours a sample of the picture holds (up to `limit`): 1 means a blank screen."""
    w, h, rows = bmp_pixels(bmp)
    seen = set()
    for y in range(0, h, step):
        line = rows[y]
        for x in range(0, w, step):
            seen.add(bytes(line[3 * x:3 * x + 3]))
            if len(seen) >= limit:
                return limit
    return len(seen)


def bmp_to_png(bmp: bytes, mask: bool = True) -> bytes:
    """PNG of a 24-bit BMP; mask=True tints the pixels outside the inscribed circle red (round panels)."""
    w, h, rows = bmp_pixels(bmp)
    raw = bytearray()
    for y, line in enumerate(rows):
        if mask:
            r2, cy = (w / 2) ** 2, y + 0.5 - h / 2
            for x in range(w):
                if (x + 0.5 - w / 2) ** 2 + cy * cy > r2:
                    line[3 * x] = 90 + line[3 * x] // 2
        raw += b"\0" + line

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def round_panel(cfg) -> bool:
    return cfg.get("screen", {}).get("shape") == "round"


def main() -> None:
    cfg = forgecfg.load()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("screen", nargs="?", default="current", help="screen name (default: current)")
    ap.add_argument("out", nargs="?", help="PNG file (default: snapshot_<screen>.png)")
    ap.add_argument("--ip", help="the display's address (default: .devloop/ip)")
    ap.add_argument("--key", help=f"the display's key (default: ${cfg['key_env']}, then .devloop/key)")
    ap.add_argument("--square", action="store_true", help="no red tint outside the round panel")
    a = ap.parse_args()
    ip = a.ip or forgecfg.read_cached(cfg, "ip")
    if not ip:
        sys.exit("The display's address: --ip IP (or run the harness once, it saves .devloop/ip)")
    key = a.key or os.environ.get(cfg["key_env"], "") or forgecfg.read_cached(cfg, "key")
    out = a.out or f"snapshot_{a.screen}.png"
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE                              # per-device self-signed certificate
    req = urllib.request.Request(forgecfg.https_base(cfg, ip) + f"/api/snapshot?screen={a.screen}",
                                 headers={"X-Key": key} if key else {})
    try:
        with urllib.request.urlopen(req, context=ctx, timeout=30) as r:
            bmp = r.read()
    except urllib.error.HTTPError as e:
        if e.code == 401:
            sys.exit(f"The display wants its key: --key KEY, ${cfg['key_env']} or .devloop/key (the test console's "
                     "'key' command, or the #k= part of the settings QR code)")
        raise
    with open(out, "wb") as f:
        f.write(bmp_to_png(bmp, round_panel(cfg) and not a.square))
    w, h, _ = bmp_pixels(bmp)
    print(f"{out}: {w}x{h}, {len(bmp)} bytes received")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Stage a build for the flash helper: copy every part ESP-IDF lists in <build_dir>/flasher_args.json to
.devloop/stage/<name>_<timestamp>.bin, check each copy by md5 and write stage/manifest.json, which the helper
checks again before it flashes (docs/PROTOCOL.md §1).

    python tools/devloop/stage.py                 # the build in forge.json's build_dir
    python tools/devloop/stage.py --build build   # another build folder

Why the unique names and the md5s: twice in the source project a rebuilt file pushed to the same path delivered the
previous version, and the board ran old firmware while the test "passed". Standard library only.
"""
import argparse
import hashlib
import json
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import forgecfg  # noqa: E402


def md5(path):
    h = hashlib.md5()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 16), b''):
            h.update(block)
    return h.hexdigest()


def flasher_args(build):
    p = os.path.join(build, 'flasher_args.json')
    if not os.path.exists(p):
        raise FileNotFoundError(f'{p} not found: build the firmware first (idf.py -B {build} build)')
    with open(p) as f:
        return json.load(f)


def stage(cfg=None, build=None):
    """Copy the parts, verify them, write the manifest; returns it. Raises on any mismatch."""
    cfg = cfg or forgecfg.load()
    build = os.path.abspath(build or forgecfg.path(cfg, cfg['build_dir']))
    fa = flasher_args(build)
    d = os.path.join(forgecfg.devloop(cfg), 'stage')
    os.makedirs(d, exist_ok=True)
    for old in os.listdir(d):                      # only the latest stage is kept: fewer files to pick wrongly
        if old.endswith('.bin') or old == 'manifest.json':
            os.remove(os.path.join(d, old))
    stamp = time.strftime('%Y%m%d-%H%M%S')
    parts = []
    for offset, rel in sorted(fa['flash_files'].items(), key=lambda kv: int(kv[0], 16)):
        src = os.path.join(build, rel)
        stem = os.path.splitext(os.path.basename(rel))[0]
        name = f'{stem}_{stamp}.bin'
        dst = os.path.join(d, name)
        shutil.copyfile(src, dst)
        want, got = md5(src), md5(dst)
        if want != got:
            raise IOError(f'staged {name} differs from {rel} (md5 {got} != {want})')
        parts.append({'offset': offset, 'file': name, 'source': rel.replace('\\', '/'), 'md5': want,
                      'size': os.path.getsize(dst)})
    manifest = {
        'created': time.strftime('%Y-%m-%dT%H:%M:%S'),
        'build_dir': os.path.relpath(build, cfg['_root']).replace('\\', '/'),
        'app': fa.get('app', {}).get('file', ''),
        'chip': fa.get('extra_esptool_args', {}).get('chip', cfg['chip']),
        'flash_settings': fa.get('flash_settings', {}),
        'parts': parts,
    }
    tmp = os.path.join(d, 'manifest.json.tmp')
    with open(tmp, 'w') as f:
        json.dump(manifest, f, indent=1)
    os.replace(tmp, os.path.join(d, 'manifest.json'))
    return manifest


def summary(m):
    lines = [f'Staged {len(m["parts"])} parts from {m["build_dir"]} ({m["chip"]}, '
             + ' '.join(f'{k}={v}' for k, v in m['flash_settings'].items()) + '):']
    lines += [f'  {p["offset"]:>9}  {p["file"]:<40} {p["size"]:>9,} B  md5 {p["md5"]}' for p in m['parts']]
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build', help="build folder with flasher_args.json (default: forge.json's build_dir)")
    a = ap.parse_args()
    try:
        print(summary(stage(build=a.build)))
    except (OSError, ValueError, KeyError) as e:
        sys.exit(f'stage: {e}')


if __name__ == '__main__':
    main()

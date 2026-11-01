#!/usr/bin/env python3
"""espforge at the tag the firmware uses, for builds outside idf.py (host tests, the emulator, CI jobs, the cloud
recipe): the whole repository at that tag, so its components, its board (boards/ws_amoled175/board) and the
emulator's framework part (web/emu/forge) are all there. The tag is the one every espforge entry of
main/idf_component.yml shares (its `version:`); the clone goes to .espforge/ (git-ignored), or to --dest. Prints the
components folder: pass it as FORGE= to the Makefiles (docs/NEW-PROJECT.md, "Later: the framework at a tag").

    python tools/fetch_forge.py                  # .espforge/components at the pinned tag
    make -C tests/host FORGE=$(python tools/fetch_forge.py)

An idf.py build puts the components (not web/emu) in managed_components/, which the host tests use first. In espforge
itself, or in an app that still has copies of the components (no espforge entry in the manifest), there is nothing to
fetch: it says so, prints the checkout's own components folder and exits 0.
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# An entry that comes from espforge's repository: git: https://github.com/TheMonkeyz/espforge(.git)
FORGE_GIT = re.compile(r'github\.com/[^/\s]+/espforge(?:\.git)?/?$', re.I)


def manifest_entries(text):
    """{name: {key: value}} for each dependency of an idf_component.yml: names at two spaces, their keys at four,
    quotes and trailing comments dropped. Enough for the manifests the apps write, without PyYAML."""
    entries, name = {}, None
    for line in text.splitlines():
        if line.lstrip().startswith('#'):
            continue
        m = re.match(r'^  (\S+):\s*(.*)$', line)
        if m:
            name = m.group(1)
            entries[name] = {}
            continue
        m = re.match(r'^    (\w+):\s*(.*)$', line)
        if m and name is not None:
            entries[name][m.group(1)] = m.group(2).split('#', 1)[0].strip().strip('"\'')
    return entries


def forge_entries(entries):
    """The entries fetched from espforge's repository, by name (forge_core, ..., board)."""
    return {n: e for n, e in entries.items() if FORGE_GIT.search(e.get('git', ''))}


def pinned_tag(entries):
    """The tag the espforge entries share; None when there is no espforge entry. They are bumped together, so two
    versions in one manifest are a mistake: SystemExit saying which."""
    forge = forge_entries(entries)
    if not forge:
        return None
    tags = sorted({e.get('version', '') for e in forge.values()})
    if len(tags) != 1 or not tags[0]:
        raise SystemExit('main/idf_component.yml: the espforge entries must share one version, found '
                         + ', '.join(f'{n} {e.get("version") or "(none)"}' for n, e in forge.items()))
    return tags[0]


def main(argv=None, root=ROOT):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dest', help='where to clone (default .espforge in the project)')
    ap.add_argument('--tag', help='another tag (default: the one pinned in main/idf_component.yml)')
    opts = ap.parse_args(argv)
    dest = opts.dest or os.path.join(root, '.espforge')
    with open(os.path.join(root, 'main', 'idf_component.yml'), encoding='utf-8') as f:
        entries = manifest_entries(f.read())
    forge = forge_entries(entries)
    if not forge:
        print('nothing to do: no espforge entry in main/idf_component.yml (this is espforge itself, or an app with '
              'copies of the components)', file=sys.stderr)
        print(os.path.join(root, 'components').replace('\\', '/'))
        return 0
    tag = opts.tag or pinned_tag(entries)
    repo = next(iter(forge.values()))['git']
    stamp = os.path.join(dest, '.forge_tag')
    have = None
    if os.path.exists(stamp):
        with open(stamp) as f:
            have = f.read().strip()
    if have != tag:
        if os.path.exists(dest):
            subprocess.run(['git', '-C', dest, 'fetch', '-q', '--depth', '1', 'origin', 'tag', tag], check=True)
            subprocess.run(['git', '-C', dest, 'checkout', '-q', tag], check=True)
        else:
            subprocess.run(['git', '-c', 'advice.detachedHead=false', 'clone', '-q', '--depth', '1', '--branch', tag,
                            repo, dest], check=True)
        with open(stamp, 'w') as f:
            f.write(tag)
        print(f'espforge {tag} in {dest}', file=sys.stderr)
    print(os.path.join(dest, 'components').replace('\\', '/'))
    return 0


if __name__ == '__main__':
    sys.exit(main())

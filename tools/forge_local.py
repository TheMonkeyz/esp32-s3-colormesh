#!/usr/bin/env python3
"""Build the app against an espforge checkout that isn't released yet: the check before tagging espforge (its
CLAUDE.md, "Shared with weather_amoled"). Nothing is committed: for the length of the build, every espforge entry of
main/idf_component.yml (git: .../espforge: the components and the board) becomes an `override_path:` entry pointing
into the checkout (the only override ESP-IDF's component manager honours over a git dependency), then the file is put
back exactly as it was, even if the build fails. The build goes to its own folder (build/forge) with its own sdkconfig;
the flash helper and the harness follow it (stage.py --build). Label the build (version.txt) as a test,
e.g. v0.4.0-forge.N.

    python tools/forge_local.py                          # against ../espforge; then flash build/forge with the harness:
    python tools/harness/harness.py --flash build/forge
    python tools/forge_local.py --forge D:/src/espforge --idf C:/Espressif/esp-idf

Run it from a shell where ESP-IDF is set up (export.ps1 / export.sh), or pass --idf. When the espforge change is good:
tag espforge (rc), bump every espforge tag in main/idf_component.yml (delete dependencies.lock), and build normally
(managed_components then holds the tagged copies again). In espforge itself, or in an app that still has copies of the
components (no espforge entry in the manifest), there is nothing to override: it says so and exits 0.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# An entry that comes from espforge's repository: git: https://github.com/TheMonkeyz/espforge(.git)
FORGE_GIT = re.compile(r'github\.com/[^/\s]+/espforge(?:\.git)?/?$', re.I)
MARK = '# tools/forge_local.py (temporary)'


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


def local_manifest(text, forge):
    """(the manifest with each espforge entry's lines replaced by one override_path into the checkout `forge`,
    {name: folder}). The entry's `path:` says where it is in espforge (components/forge_core,
    boards/ws_amoled175/board); every other line is kept."""
    nl = '\r\n' if '\r\n' in text else '\n'
    folders = {}
    for name, entry in forge_entries(manifest_entries(text)).items():
        if not entry.get('path'):
            raise SystemExit(f'{name}: no path: in main/idf_component.yml (where is it in espforge?)')
        # forward slashes whatever the PC: the component manager takes them on Windows too, and a test on Linux
        # may hand in a Windows path
        folder = os.path.join(forge, *entry['path'].split('/')).replace('\\', '/')
        block = re.compile(r'^(  ' + re.escape(name) + r':[ \t]*\r?\n)((?:    \S.*\n)+)', re.M)
        text, n = block.subn(lambda m, p=folder: m.group(1) + f'    override_path: "{p}"   {MARK}{nl}', text)
        if n != 1:
            raise SystemExit(f'{name}: its lines in main/idf_component.yml are not one block of four-space keys')
        folders[name] = folder
    return text, folders


def idf_command(idf, build, targets):
    """idf.py for the build folder, from --idf, IDF_PATH (export.ps1 / export.sh set it) or the PATH."""
    idf_path = idf or os.environ.get('IDF_PATH')
    script = os.path.join(idf_path, 'tools', 'idf.py') if idf_path else shutil.which('idf.py')
    if not script or not os.path.exists(script):
        raise SystemExit('idf.py not found: run export.ps1 / export.sh first, or pass --idf <ESP-IDF folder>')
    cmd = [sys.executable, script] if script.endswith('.py') else [script]
    return cmd + ['-B', build, '-D', 'SDKCONFIG=' + os.path.join(build, 'sdkconfig')] + list(targets)


def read(path):
    with open(path, encoding='utf-8', newline='') as f:
        return f.read()


def write(path, text):
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)


def main(argv=None, root=ROOT):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--forge', '--espforge', dest='forge',
                    help='the espforge checkout (default ../espforge, next to this project)')
    ap.add_argument('--build', default='build/forge', help='build folder (default build/forge)')
    ap.add_argument('--idf', help='ESP-IDF folder, when idf.py is not on the PATH')
    ap.add_argument('target', nargs='*', default=['build'], help='idf.py targets (default: build)')
    opts = ap.parse_args(argv)
    manifest = os.path.join(root, 'main', 'idf_component.yml')
    backup = manifest + '.forge_local'
    if os.path.exists(backup):                     # a run that was killed (Ctrl-C twice, a pipe closed early)
        shutil.move(backup, manifest)
        print('main/idf_component.yml restored from the last run that was cut short', flush=True)
    original = read(manifest)
    if not forge_entries(manifest_entries(original)):
        if MARK in original:
            raise SystemExit('main/idf_component.yml still holds the override lines of a run that was cut short, and '
                             'its backup is gone: put it back with git checkout -- main/idf_component.yml')
        print('nothing to do: no espforge entry in main/idf_component.yml (this is espforge itself, or an app with '
              'copies of the components)', flush=True)
        return 0
    forge = os.path.abspath(opts.forge or os.path.join(root, '..', 'espforge'))
    text, folders = local_manifest(original, forge)
    for name, folder in folders.items():
        if not os.path.exists(os.path.join(folder, 'CMakeLists.txt')):
            raise SystemExit(f'not an espforge checkout: {folder} has no CMakeLists.txt ({name})')
    head = subprocess.run(['git', '-C', forge, 'describe', '--tags', '--always', '--dirty'],
                          capture_output=True, text=True).stdout.strip()
    print(f'building against espforge {head or "?"} at {forge}: {", ".join(folders)}', flush=True)
    cmd = idf_command(opts.idf, opts.build, opts.target)

    write(backup, original)                        # if this script is killed: the next run copies it back
    try:
        write(manifest, text)
        code = subprocess.run(cmd, cwd=root).returncode
    finally:
        write(manifest, original)
        os.remove(backup)
    return code


if __name__ == '__main__':
    sys.exit(main())

"""tools/forge_local.py and tools/fetch_forge.py without ESP-IDF or a network: a sample manifest in a temporary project
is rewritten for the length of a (fake) idf.py run and put back byte for byte, the backup is there while it builds and
restored at the next start when a run was cut short, and in espforge itself (no espforge entry) both say there is
nothing to do and exit 0.

    python -m unittest discover -s tools/harness -p "test_*.py"
"""
import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ESPFORGE = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(HERE, '..'))
import fetch_forge  # noqa: E402
import forge_local  # noqa: E402

# An app's manifest: LVGL and another registry component untouched, three espforge entries (two components, the board)
MANIFEST = '''dependencies:
  lvgl/lvgl: "9.2.2"   # pinned: LVGL versions differ in internals the framework relies on
  # espforge's components and board, at a release tag. Bump every version together.
  forge_core:
    git: https://github.com/TheMonkeyz/espforge.git
    path: components/forge_core
    version: v0.5.0
  forge_net:
    git: https://github.com/TheMonkeyz/espforge.git
    path: components/forge_net
    version: v0.5.0
  board:
    git: https://github.com/TheMonkeyz/espforge.git
    path: boards/ws_amoled175/board
    version: v0.5.0
  espressif/esp_codec_dev: "1.5.11"
'''
COPIES = 'dependencies:\n  lvgl/lvgl: "9.2.2"   # pinned\n'     # espforge itself, or an app with copies

# A stand-in for idf.py: records its arguments, the manifest it saw and whether the backup was there, in idf_seen.json
# of the project it was run in; exits with FAKE_IDF_EXIT (default 0)
FAKE_IDF = '''import json, os, sys
seen = {"argv": sys.argv[1:], "cwd": os.getcwd(),
        "manifest": open(os.path.join("main", "idf_component.yml"), encoding="utf-8", newline="").read(),
        "backup": os.path.exists(os.path.join("main", "idf_component.yml.forge_local"))}
json.dump(seen, open("idf_seen.json", "w"))
sys.exit(int(os.environ.get("FAKE_IDF_EXIT", "0")))
'''


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)


def read(path):
    with open(path, encoding='utf-8', newline='') as f:
        return f.read()


class TempProject(unittest.TestCase):
    """An app in a temporary folder, a fake espforge checkout next to it (../espforge), a fake ESP-IDF."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.root = os.path.join(self.tmp, 'app')
        self.manifest = os.path.join(self.root, 'main', 'idf_component.yml')
        write(self.manifest, MANIFEST)
        self.forge = os.path.join(self.tmp, 'espforge')
        for rel in ('components/forge_core', 'components/forge_net', 'boards/ws_amoled175/board'):
            write(os.path.join(self.forge, *rel.split('/'), 'CMakeLists.txt'), '# fake\n')
        self.idf = os.path.join(self.tmp, 'esp-idf')
        write(os.path.join(self.idf, 'tools', 'idf.py'), FAKE_IDF)
        os.environ.pop('FAKE_IDF_EXIT', None)

    def tearDown(self):
        os.environ.pop('FAKE_IDF_EXIT', None)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_local(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = forge_local.main(['--idf', self.idf, *args], root=self.root)
        return code, out.getvalue()

    def seen(self):
        with open(os.path.join(self.root, 'idf_seen.json')) as f:
            return json.load(f)


class LocalManifest(unittest.TestCase):
    def test_every_espforge_entry_becomes_an_override_path_from_the_manifest(self):
        text, folders = forge_local.local_manifest(MANIFEST, '/src/espforge')
        self.assertEqual(folders, {'forge_core': '/src/espforge/components/forge_core',
                                   'forge_net': '/src/espforge/components/forge_net',
                                   'board': '/src/espforge/boards/ws_amoled175/board'})
        self.assertIn('  board:\n    override_path: "/src/espforge/boards/ws_amoled175/board"   '
                      '# tools/forge_local.py (temporary)\n', text)
        self.assertNotIn('git:', text)
        self.assertNotIn('version: v0.5.0', text)
        self.assertEqual(text.count('override_path'), 3)
        for kept in ('  lvgl/lvgl: "9.2.2"   # pinned', "  # espforge's components and board",
                     '  espressif/esp_codec_dev: "1.5.11"'):
            self.assertIn(kept, text, 'a line that is not an espforge entry was changed')

    def test_a_windows_path_and_crlf_lines(self):
        text, folders = forge_local.local_manifest(MANIFEST.replace('\n', '\r\n'), 'C:\\src\\espforge')
        self.assertEqual(folders['forge_core'], 'C:/src/espforge/components/forge_core')
        self.assertIn('    override_path: "C:/src/espforge/components/forge_core"   # tools/forge_local.py (temporary)\r\n',
                      text)
        self.assertNotIn('\n\n', text.replace('\r\n', '\n') + 'x', 'a line was lost or doubled')

    def test_an_entry_without_a_path_is_refused(self):
        with self.assertRaises(SystemExit) as cm:
            forge_local.local_manifest(MANIFEST.replace('    path: components/forge_net\n', ''), '/src/espforge')
        self.assertIn('forge_net', str(cm.exception))

    def test_a_manifest_with_copies_has_no_espforge_entry(self):
        self.assertEqual(forge_local.forge_entries(forge_local.manifest_entries(COPIES)), {})
        text, folders = forge_local.local_manifest(COPIES, '/src/espforge')
        self.assertEqual((text, folders), (COPIES, {}))


class Build(TempProject):
    def test_the_manifest_is_overridden_for_the_build_and_put_back(self):
        code, out = self.run_local('--forge', self.forge)
        self.assertEqual(code, 0)
        self.assertIn('building against espforge', out)
        seen = self.seen()
        self.assertEqual(seen['argv'][:3], ['-B', 'build/forge', '-D'])
        self.assertEqual(seen['argv'][3], 'SDKCONFIG=' + os.path.join('build/forge', 'sdkconfig'))
        self.assertEqual(seen['argv'][4:], ['build'])
        # realpath: macOS's temp folder is a symlink (/var -> /private/var), so idf.py's getcwd() differs in name
        self.assertEqual(os.path.normcase(os.path.realpath(seen['cwd'])),
                         os.path.normcase(os.path.realpath(self.root)))
        self.assertIn('override_path: "' + self.forge.replace(os.sep, '/') + '/components/forge_core"', seen['manifest'])
        self.assertIn('override_path: "' + self.forge.replace(os.sep, '/') + '/boards/ws_amoled175/board"',
                      seen['manifest'])
        self.assertNotIn('git:', seen['manifest'])
        self.assertTrue(seen['backup'], 'no backup while idf.py ran')
        self.assertEqual(read(self.manifest), MANIFEST, 'the manifest was not put back byte for byte')
        self.assertFalse(os.path.exists(self.manifest + '.forge_local'), 'the backup outlived the run')

    def test_a_failed_build_still_puts_the_manifest_back(self):
        os.environ['FAKE_IDF_EXIT'] = '7'
        code, _ = self.run_local('--forge', self.forge, 'build', 'size')
        self.assertEqual(code, 7)
        self.assertEqual(self.seen()['argv'][4:], ['build', 'size'])
        self.assertEqual(read(self.manifest), MANIFEST)
        self.assertFalse(os.path.exists(self.manifest + '.forge_local'))

    def test_the_default_checkout_is_next_to_the_project(self):
        code, out = self.run_local()           # ../espforge from the app: the fake checkout is there
        self.assertEqual(code, 0)
        self.assertIn(os.path.normcase(self.forge), os.path.normcase(out))

    def test_a_backup_left_by_a_killed_run_is_restored_first(self):
        write(self.manifest + '.forge_local', MANIFEST)
        write(self.manifest, forge_local.local_manifest(MANIFEST, '/elsewhere')[0])   # what a killed run leaves
        code, out = self.run_local('--forge', self.forge)
        self.assertEqual(code, 0)
        self.assertIn('restored from the last run that was cut short', out)
        self.assertIn('override_path: "' + self.forge.replace(os.sep, '/'), self.seen()['manifest'])
        self.assertEqual(read(self.manifest), MANIFEST)

    def test_override_lines_without_a_backup_are_refused(self):
        write(self.manifest, forge_local.local_manifest(MANIFEST, '/elsewhere')[0])
        with self.assertRaises(SystemExit) as cm:
            self.run_local('--forge', self.forge)
        self.assertIn('git checkout', str(cm.exception))

    def test_not_an_espforge_checkout(self):
        shutil.rmtree(os.path.join(self.forge, 'boards'))
        with self.assertRaises(SystemExit) as cm:
            self.run_local('--forge', self.forge)
        self.assertIn('not an espforge checkout', str(cm.exception))
        self.assertIn('board', str(cm.exception))
        self.assertEqual(read(self.manifest), MANIFEST)
        self.assertFalse(os.path.exists(self.manifest + '.forge_local'))

    def test_nothing_to_do_with_copies(self):
        write(self.manifest, COPIES)
        code, out = self.run_local('--forge', self.forge)
        self.assertEqual(code, 0)
        self.assertIn('nothing to do', out)
        self.assertFalse(os.path.exists(os.path.join(self.root, 'idf_seen.json')), 'idf.py was run')
        self.assertEqual(read(self.manifest), COPIES)
        self.assertFalse(os.path.exists(self.manifest + '.forge_local'))

    def test_nothing_to_do_in_espforge_itself(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = forge_local.main(['--idf', self.idf], root=ESPFORGE)
        self.assertEqual(code, 0)
        self.assertIn('nothing to do', out.getvalue())
        self.assertFalse(os.path.exists(os.path.join(ESPFORGE, 'main', 'idf_component.yml.forge_local')))


class FetchForge(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.root = os.path.join(self.tmp, 'app')
        self.manifest = os.path.join(self.root, 'main', 'idf_component.yml')
        self.old_run = fetch_forge.subprocess.run
        fetch_forge.subprocess.run = lambda *a, **k: self.fail(f'git was run: {a[0]}')

    def tearDown(self):
        fetch_forge.subprocess.run = self.old_run
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fetch(self, *args, root=None):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = fetch_forge.main(list(args), root=root or self.root)
        return code, out.getvalue().strip(), err.getvalue()

    def test_the_pinned_tag_is_the_one_every_espforge_entry_shares(self):
        self.assertEqual(fetch_forge.pinned_tag(fetch_forge.manifest_entries(MANIFEST)), 'v0.5.0')
        self.assertIsNone(fetch_forge.pinned_tag(fetch_forge.manifest_entries(COPIES)))
        entries = fetch_forge.manifest_entries(MANIFEST.replace('path: components/forge_net\n    version: v0.5.0',
                                                                'path: components/forge_net\n    version: v0.6.0'))
        with self.assertRaises(SystemExit) as cm:
            fetch_forge.pinned_tag(entries)
        self.assertIn('forge_net v0.6.0', str(cm.exception))
        self.assertIn('forge_core v0.5.0', str(cm.exception))

    def test_entries_are_read_with_quotes_and_comments_dropped(self):
        entries = fetch_forge.manifest_entries(MANIFEST)
        self.assertEqual(entries['lvgl/lvgl'], {})
        self.assertEqual(entries['board'], {'git': 'https://github.com/TheMonkeyz/espforge.git',
                                            'path': 'boards/ws_amoled175/board', 'version': 'v0.5.0'})
        self.assertEqual(sorted(fetch_forge.forge_entries(entries)), ['board', 'forge_core', 'forge_net'])

    def test_a_clone_already_at_the_tag_is_kept(self):
        write(self.manifest, MANIFEST)
        dest = os.path.join(self.root, '.espforge')
        write(os.path.join(dest, '.forge_tag'), 'v0.5.0\n')
        code, out, err = self.run_fetch()
        self.assertEqual(code, 0)
        self.assertEqual(out, dest.replace('\\', '/') + '/components')
        self.assertEqual(err, '')

    def test_another_dest(self):
        write(self.manifest, MANIFEST)
        dest = os.path.join(self.tmp, 'elsewhere')
        write(os.path.join(dest, '.forge_tag'), 'v0.5.0')
        code, out, _ = self.run_fetch('--dest', dest)
        self.assertEqual((code, out), (0, dest.replace('\\', '/') + '/components'))

    def test_nothing_to_do_with_copies(self):
        write(self.manifest, COPIES)
        code, out, err = self.run_fetch()
        self.assertEqual(code, 0)
        self.assertIn('nothing to do', err)
        self.assertEqual(out, self.root.replace('\\', '/') + '/components', 'the checkout\'s own components folder')

    def test_nothing_to_do_in_espforge_itself(self):
        code, out, err = self.run_fetch(root=ESPFORGE)
        self.assertEqual(code, 0)
        self.assertIn('nothing to do', err)
        self.assertTrue(os.path.isdir(out), out)
        self.assertEqual(os.path.basename(out), 'components')


if __name__ == '__main__':
    unittest.main()

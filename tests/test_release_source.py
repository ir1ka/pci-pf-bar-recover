"""Exercise source export using disposable repositories, never the real checkout."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / 'tools/release_source.py'


class SourceArchiveTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)
        self.root = self.base / 'repo'
        self.init_repo(self.root)
        (self.root / 'tools').mkdir()
        shutil.copyfile(TOOL, self.root / 'tools/release_source.py')
        (self.root / 'release.json').write_text('{"name":"fixture"}')
        (self.root / 'hello').write_text('source\n')
        (self.root / 'alias').symlink_to('hello')
        self.commit(self.root)

    def tearDown(self):
        self.tmp.cleanup()

    def run_git(self, root, *args):
        return subprocess.check_output(['git', '-C', str(root), *args], stderr=subprocess.DEVNULL)

    def init_repo(self, root):
        root.mkdir()
        self.run_git(root, 'init', '-q')
        self.run_git(root, 'config', 'user.name', 'Fixture')
        self.run_git(root, 'config', 'user.email', 'fixture@example.invalid')

    def commit(self, root):
        self.run_git(root, 'add', '.')
        self.run_git(root, 'commit', '-qm', 'fixture')

    def export(self, directory, ok=True):
        result = subprocess.run([sys.executable, str(self.root / 'tools/release_source.py'),
                                 '--version', '1.0', '--output', str(directory)],
                                capture_output=True)
        self.assertEqual(result.returncode == 0, ok, result.stderr.decode())
        return directory / 'fixture-1.0.tar.gz'

    def test_reproducible_manifest_and_symlink(self):
        first = self.export(self.base / 'one')
        second = self.export(self.base / 'two')
        self.assertEqual(first.read_bytes(), second.read_bytes())
        with tarfile.open(first) as archive:
            manifest = json.load(archive.extractfile('fixture-1.0/SOURCE-MANIFEST.json'))
            self.assertEqual(manifest['files']['hello'], hashlib.sha256(b'source\n').hexdigest())
            self.assertTrue(archive.getmember('fixture-1.0/alias').issym())
            self.assertFalse(any('/.git/' in n for n in archive.getnames()))

    def test_dirty_tree_and_overwrite_rejected(self):
        self.export(self.base / 'one')
        self.export(self.base / 'one', ok=False)
        (self.root / 'hello').write_text('dirty')
        self.export(self.base / 'two', ok=False)

    def test_pinned_submodule_included(self):
        child = self.base / 'child'
        self.init_repo(child)
        (child / 'LICENSE').write_text('fixture license')
        self.commit(child)
        self.run_git(self.root, '-c', 'protocol.file.allow=always', 'submodule', 'add', '-q', str(child), 'vendor/child')
        self.commit(self.root)
        output = self.export(self.base / 'one')
        with tarfile.open(output) as archive:
            self.assertEqual(archive.extractfile('fixture-1.0/vendor/child/LICENSE').read(), b'fixture license')
            manifest = json.load(archive.extractfile('fixture-1.0/SOURCE-MANIFEST.json'))
            self.assertIn('vendor/child/', manifest['revisions'])
        shutil.rmtree(self.root / 'vendor/child')
        self.export(self.base / 'two', ok=False)

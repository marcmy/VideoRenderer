"""Exercise ordinary upstream edits against the fork's actual source layout.

The former migration wrappers caused delete/modify conflicts even when upstream
changed an unrelated declaration. Use Git's real three-way file merger to catch
that regression, and ensure genuinely overlapping edits still require review.
No checkout, index, branch, network, or renderer state is modified.
"""
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
UPSTREAM = json.loads((ROOT / 'upstream.version.json').read_text())['commit']
PROBES = {
    'Source/DX11VideoProcessor.h': '\tbool Initialized();',
    'Source/VideoRenderer.h': '\tvoid UpdateDisplayInfo();',
    'Source/VideoRenderer.cpp': 'static const wchar_t g_szClassName[] = L"VRWindow";',
}


def upstream_source(path):
    return subprocess.check_output(['git', '-C', str(ROOT), 'show', UPSTREAM + ':' + path]).decode().replace('\r\n', '\n')


def merge(ours, base, theirs):
    with tempfile.TemporaryDirectory(prefix='mpcvr-upstream-merge-') as directory:
        paths = [Path(directory) / name for name in ('ours', 'base', 'theirs')]
        for path, text in zip(paths, (ours, base, theirs)):
            path.write_bytes(text.encode())
        result = subprocess.run(['git', 'merge-file', '--stdout', '--diff3', *map(str, paths)], capture_output=True)
        if result.returncode < 0 or result.returncode > 127:
            raise RuntimeError(result.stderr.decode())
        return result.returncode, result.stdout.decode()


class UpstreamMergeTests(unittest.TestCase):
    def test_unrelated_upstream_edits_preserve_all_fork_source(self):
        for path, anchor in PROBES.items():
            with self.subTest(path=path):
                base = upstream_source(path)
                self.assertEqual(base.count(anchor), 1, 'Upstream probe needs a unique declaration')
                ours = (ROOT / path).read_text()
                changed = anchor + ' // upstream merge probe'
                theirs = base.replace(anchor, changed)
                status, merged = merge(ours, base, theirs)
                self.assertEqual(status, 0, 'Unrelated upstream edit conflicts with the fork layout: ' + path)
                self.assertIn(changed, merged)
                self.assertEqual(merged.replace(changed, anchor), ours, 'Merge discarded or altered fork source')

    def test_overlapping_source_edits_still_require_review(self):
        for path, anchor in PROBES.items():
            with self.subTest(path=path):
                base = upstream_source(path)
                ours = (ROOT / path).read_text()
                self.assertEqual(ours.count(anchor), 1, 'Fork should retain the upstream declaration')
                ours = ours.replace(anchor, anchor + ' // fork change')
                theirs = base.replace(anchor, anchor + ' // incompatible upstream change')
                status, merged = merge(ours, base, theirs)
                self.assertGreater(status, 0, 'Overlapping edits were silently accepted')
                self.assertIn('<<<<<<<', merged)
                self.assertIn('// fork change', merged)
                self.assertIn('// incompatible upstream change', merged)


if __name__ == '__main__':
    unittest.main()

import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('stage_pascal_pack', Path(__file__).with_name('stage_pascal_pack.py'))
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)


class InventoryTests(unittest.TestCase):
    def test_runtime_and_installer_pins_agree(self):
        manifest = json.loads(stage.MANIFEST.read_text())
        root = stage.MANIFEST.parents[1]
        header = (root / 'src/FrameGen/NeuralAdmission.h').read_text()
        self.assertIn(manifest['sha256'], header)
        self.assertIn(Path(manifest['path']).name, header)
        self.assertIn(manifest['requires']['sha256'], (root / 'src/FrameGen/NeuralRuntimeKernels.h').read_text())
        self.assertEqual(manifest['architecture'], 61)
        self.assertEqual(len(manifest['retainedUniversalAssets']), 2)

    def test_standard_never_stages_and_tamper_refuses(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / 'fake.trpk'
            source.write_bytes(b'not the pinned pack')
            for edition in ['Standard', 'Universal']:
                with self.assertRaises(ValueError):
                    stage.stage(source, root / edition, edition)
                self.assertFalse((root / edition).exists())


if __name__ == '__main__':
    unittest.main()

"""Run the real Windows launcher script with preparation stand-ins; no game required."""
from paths import ROOT
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(os.name == 'nt', 'Windows batch launcher')
class ResolutionMemoryTests(unittest.TestCase):
    def memory_at_patch_compilation(self, resolution, memory=None):
        with tempfile.TemporaryDirectory(prefix='bb memory ') as directory:
            root = Path(directory)
            shutil.copyfile(ROOT / 'run.bat', root / 'run.bat')
            (root / 'eboot.bin').touch()
            scripts = root / 'scripts'
            scripts.mkdir()
            for name in ('mods', 'prepare', 'link_libc', 'link_modules', 'content_profile'):
                (scripts / f'{name}.py').write_text('pass\n')
            (scripts / 'patches.py').write_text(
                'import json, os, sys\n'
                'from pathlib import Path\n'
                'if "--print-scaled" not in sys.argv:\n'
                '    Path("observed.json").write_text(json.dumps(dict(os.environ)))\n'
                '    sys.exit(73) # Stop before the real executable can be launched.\n')
            env = {k: v for k, v in os.environ.items() if not k.upper().startswith('BB_')}
            env.update(PYTHON=sys.executable, BB_GAME_DIR=str(root), BB_DATA_DIR=str(root))
            if resolution is not None:
                env['BB_RENDER_RES'] = resolution
            if memory is not None:
                env['BB_DMEM_MB'] = memory
            run = subprocess.run([os.environ['COMSPEC'], '/d', '/c', 'run.bat'],
                                 cwd=root, env=env, capture_output=True, timeout=30)
            self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
            self.assertTrue((root / 'observed.json').exists(), run.stdout + run.stderr)
            return json.loads((root / 'observed.json').read_text()).get('BB_DMEM_MB')

    def test_explicit_above_1080p_gets_extra_memory(self):
        for resolution in ('2560x1440', '3840x2160', '3840X2160', '2560x1080'):
            with self.subTest(resolution=resolution):
                self.assertEqual(self.memory_at_patch_compilation(resolution), '9152')

    def test_default_and_lower_resolutions_keep_default_pool(self):
        for resolution in (None, '1280x720', '1920x1080'):
            with self.subTest(resolution=resolution):
                self.assertIsNone(self.memory_at_patch_compilation(resolution))

    def test_explicit_memory_budget_is_preserved(self):
        self.assertEqual(self.memory_at_patch_compilation('3840x2160', '12000'), '12000')


if __name__ == '__main__':
    unittest.main()

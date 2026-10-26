"""Regression tests for generated headers used by incremental builds."""
import json
import os
from pathlib import Path
import runpy
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

PROJECT = Path(__file__).resolve().parents[2]


class BuildEnv(dict):
    def __init__(self, root):
        super().__init__(PROJECT_SRC_DIR=str(root / 'src'), BUILD_FLAGS=['-DESP32=1'])
        self.root = root

    def GetProjectOption(self, name, default):
        return default

    def subst(self, value):
        assert value == '$PROJECT_DIR'
        return str(self.root)


def execute(script, root):
    return runpy.run_path(str(PROJECT / script), init_globals={
        'Import': lambda name: None, 'env': BuildEnv(root)})


class GeneratorTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / 'src').mkdir()

    def preserve_marker(self, output, regenerate):
        content = output.read_bytes()
        os.utime(output, ns=(1000000000, 1000000000))
        regenerate()
        self.assertEqual(output.read_bytes(), content)
        self.assertEqual(output.stat().st_mtime_ns, 1000000000)

    def test_version_changes_only_its_header(self):
        result = SimpleNamespace(returncode=0, stdout='v1-test\n')
        with patch('subprocess.run', return_value=result):
            execute('auto_firmware_version.py', self.root)
            output = self.root / 'include/system/firmware_version_gen.hpp'
            self.preserve_marker(output, lambda: execute('auto_firmware_version.py', self.root))
        with patch('subprocess.run', return_value=SimpleNamespace(returncode=0, stdout='v2-test\n')):
            execute('auto_firmware_version.py', self.root)
        self.assertIn('v2-test', output.read_text())

    def test_build_stamp_reuses_unchanged_inputs(self):
        source = self.root / 'src/main.cpp'
        source.write_text('int value = 1;\n')
        with patch('time.time', return_value=100):
            module = execute('scripts/generate_build_info.py', self.root)
        output = self.root / 'include/app/build_info_gen.hpp'
        generate = module['generate']
        flags = repr(BuildEnv(self.root)['BUILD_FLAGS']).encode()
        with patch('time.time', return_value=200):
            self.preserve_marker(output, lambda: generate(self.root, flags))
        self.assertIn('build_epoch = 100LL', output.read_text())
        source.write_text('int value = 2;\n')
        with patch('time.time', return_value=300):
            generate(self.root, flags)
        self.assertIn('build_epoch = 300LL', output.read_text())
        with patch('time.time', return_value=400):
            generate(self.root, b'different flags')
        self.assertIn('build_epoch = 400LL', output.read_text())

    def test_local_library_changes_update_build_stamp(self):
        library = self.root / 'local_library'
        (library / 'src').mkdir(parents=True)
        source = library / 'src/library.cpp'
        source.write_text('int value = 1;')
        module = execute('scripts/generate_build_info.py', self.root)
        generate = module['generate']
        output = self.root / 'include/app/build_info_gen.hpp'
        with patch('time.time', return_value=100):
            generate(self.root, b'', [library])
        self.preserve_marker(output, lambda: generate(self.root, b'', [library]))
        source.write_text('int value = 2;')
        with patch('time.time', return_value=200):
            generate(self.root, b'', [library])
        self.assertIn('build_epoch = 200LL', output.read_text())

    def test_profiles_are_only_rewritten_when_content_changes(self):
        profiles = self.root / 'profiles'
        profiles.mkdir()
        for kind in ('data', 'ha'):
            with self.subTest(kind=kind):
                base = PROJECT / f'profiles/{kind}_profiles.json'
                data = json.loads(base.read_text())[:1]
                source = profiles / base.name
                source.write_text(json.dumps(data))
                script = f'scripts/generate_{kind}_profiles.py'
                execute(script, self.root)
                output = self.root / f'include/app/{kind}_profile_gen.hpp'
                self.preserve_marker(output, lambda: execute(script, self.root))
                data[0]['name'] = 'changed_profile'
                source.write_text(json.dumps(data))
                execute(script, self.root)
                self.assertIn('changed_profile', output.read_text())


if __name__ == '__main__':
    unittest.main()

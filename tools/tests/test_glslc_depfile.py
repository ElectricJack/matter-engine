import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('depfile', Path(__file__).parents[1] / 'normalize-glslc-depfile.py')
depfile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(depfile)


class TestDepfile(unittest.TestCase):
    def test_cross_drive_spaced_source_and_nested_include(self):
        paths = ['D:/Shared With Desktop/shaders/a.comp', 'D:/Shared With Desktop/shaders/nested/b.glsl']
        result = depfile.normalize('out.spv: ' + ' '.join(paths) + '\n', lambda name: name in paths)
        self.assertEqual(result, 'out.spv: D\\:/Shared\\ With\\ Desktop/shaders/a.comp '
                         'D\\:/Shared\\ With\\ Desktop/shaders/nested/b.glsl\n')

    def test_missing_or_relative_dependency_rejected(self):
        for content in ['out.spv: relative.glsl', 'out.spv: D:/missing file.glsl']:
            with self.assertRaises(ValueError):
                depfile.normalize(content, lambda name: False)

    def test_make_special_characters(self):
        self.assertEqual(depfile.escape('C:/a $/#x'), 'C\\:/a\\ $$/\\#x')


if __name__ == '__main__':
    unittest.main()

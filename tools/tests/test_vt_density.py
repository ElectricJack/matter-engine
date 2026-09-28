import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("vt_density", Path(__file__).parents[1] / "vt_density.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

SNAPSHOT = """[vt-density] begin schema=1 frame=99 capacity=8 occupied=2 pinned=1 format_bytes=1035776 payload=128 stride=136
[vt-density-page] slot=0 owner=abcdef generation=1 mip=1 x=0 y=0 pinned=1 tail_filled=1 geometry=1 atlas=4096 block=4096 bounds=64 gutter=144 triangle=64
[vt-density-page] slot=1 owner=abcdef generation=2 mip=0 x=0 y=0 pinned=0 tail_filled=1 geometry=1 atlas=16384 block=16384 bounds=256 gutter=576 triangle=136
[vt-density] end frame=99 reported=2 occupied=2
"""


class DensityTests(unittest.TestCase):
    def test_partition_and_separate_tails(self):
        result = module.analyze(SNAPSHOT)
        self.assertEqual(result["occupied_fraction"], 0.25)
        groups = result["groups"]
        self.assertEqual(groups["all"]["triangle_covered_texels"], 200)
        self.assertEqual(groups["pinned"]["triangle_covered_texels"], 64)
        self.assertEqual(groups["detail"]["triangle_covered_texels"], 136)
        for group in groups.values():
            self.assertEqual(sum(group["payload_partition"].values()), group["payload_texels"])

    def test_reject_incomplete_duplicate_and_invalid_bounds(self):
        cases = [SNAPSHOT.rsplit("[vt-density] end", 1)[0],
                 SNAPSHOT.replace("reported=2", "reported=1"),
                 SNAPSHOT.replace("slot=1", "slot=0"),
                 SNAPSHOT.replace("triangle=136", "triangle=400"),
                 SNAPSHOT + SNAPSHOT]
        for text in cases:
            with self.subTest(text=text), self.assertRaises(ValueError):
                module.analyze(text)

    def test_missing_geometry_remains_unknown(self):
        result = module.analyze(SNAPSHOT.replace("geometry=1", "geometry=0", 1))
        self.assertIsNone(result["groups"]["all"]["triangle_covered_texels"])
        self.assertEqual(result["groups"]["detail"]["triangle_covered_texels"], 136)


if __name__ == "__main__":
    unittest.main()

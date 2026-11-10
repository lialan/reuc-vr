"""Cache migration must not turn an all-indirect recording into direct stages."""
import importlib.util
import pathlib
import sys
import unittest

root = pathlib.Path(sys.argv.pop(1))
spec = importlib.util.spec_from_file_location('fold', root / 'tools/fold_tev_shaders.py')
fold = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fold)


class FoldKeys(unittest.TestCase):
    def test_recorded_indirect_stage(self):
        # These are serialized fields, independent of the C++ encoder. The
        # ordinary TEV selectors are 0x148 and 0x2ab; command 0x103601,
        # selected map 3, texcoord 5. There are no direct stages in this key.
        key = [0] * 70
        key[1], key[5], key[6] = 1, 0x00AE0148, 0x40D806AB
        key = tuple(key)
        self.assertEqual(fold.headset_key(key), key)
        generic = fold.stripped_key(key)
        self.assertEqual(generic[5:7], (0x148, 0x2AB))
        self.assertNotEqual(fold.name(fold.constants(key), 0), fold.name(fold.constants(generic), 0))
        self.assertEqual(fold.stripped_key(fold.headset_key(key)), generic)

    def test_legacy_and_mixed_keys(self):
        legacy = [0] * 70
        legacy[1], legacy[5], legacy[6] = 2, 0x148, 0x2AB
        legacy[9], legacy[10] = 0x50, 0x155
        migrated = fold.headset_key(tuple(legacy))
        self.assertEqual(migrated[5], 0x10148)
        self.assertEqual(migrated[9], 0x10050)
        self.assertEqual(fold.stripped_key(migrated), tuple(legacy))
        mixed = list(migrated)
        mixed[5], mixed[6] = 0x00AE0148, 0x40D806AB
        self.assertEqual(fold.headset_key(tuple(mixed)), tuple(mixed))
        self.assertEqual(fold.stripped_key(tuple(mixed)), tuple(legacy))


    def test_affine_coordinate_variant(self):
        for stages in (0, 1):
            key = [0] * 70
            key[1], key[5], key[6] = stages, 0x10148 if stages else 0, 0x2AB if stages else 0
            key = tuple(key)
            affine = fold.affine_coordinates_key(key)
            self.assertEqual(affine[5], key[5] | 0x1000000)
            self.assertEqual(fold.general_coordinates_key(affine), key)
            self.assertEqual(fold.stripped_key(affine), fold.stripped_key(key))
            self.assertNotEqual(fold.name(fold.constants(affine), 0), fold.name(fold.constants(key), 0))
        key = [0] * 70
        key[1] = 1
        self.assertEqual(fold.affine_coordinates_key(key), tuple(key))


unittest.main()

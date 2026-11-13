"""Comparator safety checks; no game data or running game needed."""
from pathlib import Path
import tempfile
import unittest

from PIL import Image, ImageDraw
from kingdom_tour import comparison


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.reference = self.root / 'reference.png'
        self.candidate = self.root / 'candidate.png'
        self.diff = self.root / 'diff.png'
        Image.new('RGB', (1280, 720)).save(self.reference)

    def test_identical_images_are_within_threshold(self):
        result = comparison(self.reference, self.reference, self.diff)
        self.assertEqual(result['comparison'], 'within_threshold')
        self.assertEqual(result['max_tile_difference'], 0)
        self.assertTrue(self.diff.is_file())

    def test_small_artifact_is_not_hidden_by_screen_average(self):
        image = Image.new('RGB', (1280, 720))
        ImageDraw.Draw(image).rectangle((64, 64, 95, 95), fill='white')
        image.save(self.candidate)
        result = comparison(self.candidate, self.reference, self.diff)
        self.assertLess(result['mean_absolute_difference'], 0.01)
        self.assertEqual(result['comparison'], 'review')
        self.assertEqual(result['max_tile_difference'], 1)

    def test_different_capture_dimensions_require_review(self):
        Image.new('RGB', (640, 360)).save(self.candidate)
        result = comparison(self.candidate, self.reference, self.diff)
        self.assertEqual(result['comparison'], 'size_mismatch')


if __name__ == '__main__':
    unittest.main()

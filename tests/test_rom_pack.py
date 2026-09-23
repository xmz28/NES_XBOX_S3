import csv
import tempfile
import unittest
from pathlib import Path

from tools.build_rom_pack import build_pack, ines_info


PROJECT_DIR = Path(__file__).resolve().parents[1]
TEST_ROM = PROJECT_DIR / "assets" / "test.nes"


class RomPackTests(unittest.TestCase):
    def test_original_test_rom_is_valid_nrom(self) -> None:
        mapper, expected_size = ines_info(TEST_ROM.read_bytes(), TEST_ROM)
        self.assertEqual(mapper, 0)
        self.assertEqual(expected_size, TEST_ROM.stat().st_size)

    def test_builds_single_rom_public_pack(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = root / "manifest.csv"
            output = root / "roms.bin"
            with manifest.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.writer(handle)
                writer.writerow(("display_name", "source_file"))
                writer.writerow(("NES TEST", str(TEST_ROM)))

            count, size = build_pack(manifest, root, output, 0xE70000)

            self.assertEqual(count, 1)
            self.assertEqual(output.stat().st_size, size)
            self.assertEqual(output.read_bytes()[:8], b"NESPACK1")


if __name__ == "__main__":
    unittest.main()

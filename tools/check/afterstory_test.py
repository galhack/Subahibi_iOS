"""Source-only regression checks; no game data or font fixtures required."""
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/patch"))
from compatible_font import scenario_strings
from build_afterstory_patch import check_delta, RESOURCES

spec = importlib.util.spec_from_file_location("push_data", ROOT / "tools/push_data.py")
push = importlib.util.module_from_spec(spec)
spec.loader.exec_module(push)


class AfterstoryTest(unittest.TestCase):
    def test_parser_returns_string_offset_without_matching_code_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "extra"
            path.write_bytes(b"BurikoCompiledScriptVer1.00\0" + struct.pack("<II", 8, 0)
                             + struct.pack("<IIII", 0, 0x4C81, 3, 16) + b"\x81\x4c\0")
            self.assertEqual(scenario_strings(path), [(0x24 + 16, b"\x81\x4c")])

    def test_rejects_string_reference_outside_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "extra"
            path.write_bytes(b"BurikoCompiledScriptVer1.00\0" + struct.pack("<IIII", 8, 0, 3, 9999))
            with self.assertRaises(ValueError):
                scenario_strings(path)

    def test_only_afterstory_changes_are_accepted(self):
        with tempfile.TemporaryDirectory() as temporary:
            old, new = Path(temporary) / "old", Path(temporary) / "new"
            old.mkdir()
            new.mkdir()
            for folder in (old, new):
                (folder / "Main").write_bytes(b"same")
                (folder / "8_af").write_bytes(str(folder).encode())
            for name in RESOURCES - {"8_af"}:
                (new / name).write_bytes(b"added")
            check_delta(old, new)
            (new / "Main").write_bytes(b"changed")
            with self.assertRaises(ValueError):
                check_delta(old, new)

    def test_patch_only_transfer_sends_manifest_last_and_excludes_base(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            patch = folder / "AfterstoryCHS"
            patch.mkdir()
            (folder / "font.ttf").write_bytes(b"base")
            for name in push.PATCH_FILES:
                (patch / name).write_bytes(b"delta")
            (patch / "Main").write_bytes(b"must not send")
            wanted = push.wanted(folder, patch_only=True)
            self.assertEqual(set(wanted), {"AfterstoryCHS/" + name for name in push.PATCH_FILES})
            self.assertEqual(wanted[-1], "AfterstoryCHS/patch.json")


if __name__ == "__main__":
    unittest.main()

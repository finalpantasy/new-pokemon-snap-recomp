"""Unit tests for scripts/mods/make_override.py (path mapping). Run: python -I tests/mods/test_make_override.py"""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts" / "mods"))
import make_override as mo  # noqa: E402


class T(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.ex = self.tmp / "extracted"
        (self.ex / "common/UI/master/Foo.drp").mkdir(parents=True)
        self.entry = self.ex / "common/UI/master/Foo.drp/icon_a.nutexb"
        self.entry.write_bytes(b"orig")
        self.rep = self.tmp / "new.nutexb"
        self.rep.write_bytes(b"x" * 400)

    def test_relpath(self):
        self.assertEqual(mo.mod_relpath(self.entry, self.ex), "common/UI/master/Foo.drp/icon_a.nutexb")
        with self.assertRaises(ValueError):
            mo.mod_relpath(self.ex / "common/UI/readme.txt", self.ex)

    def test_place(self):
        pack = self.tmp / "mods" / "p"
        dest, notes = mo.place(self.entry, self.rep, pack, self.ex)
        self.assertEqual(dest, pack / "common/UI/master/Foo.drp/icon_a.nutexb")
        self.assertEqual(dest.stat().st_size, 400)   # any size is accepted
        self.assertTrue((pack / "pack.json").exists())
        self.assertEqual(notes, [])

    def test_dup_and_ext_notes(self):
        e2 = self.ex / "common/UI/master/Foo.drp/icon__12.nutexb"
        e2.write_bytes(b"o")
        rep = self.tmp / "new.png"
        rep.write_bytes(b"p")
        _, notes = mo.place(e2, rep, self.tmp / "mods" / "q", self.ex, dry_run=True)
        self.assertEqual(len(notes), 2)
        self.assertFalse((self.tmp / "mods" / "q").exists())


if __name__ == "__main__":
    unittest.main()

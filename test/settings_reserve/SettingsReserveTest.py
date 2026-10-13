"""Guards the SettingInfo list builders against growing past their reserve.

A SettingInfo is large (std::function members, vectors), so a list that outgrows
its reserve() reallocates to roughly double size while the previous list is
still alive. On the device heap that contiguous block can fail and throw
std::bad_alloc from vector::emplace_back (seen as a crash when toggling a
setting on the Apps tab). The simulator and host never fail that allocation, so
check the row count against the reserve statically.

Row counts are an upper bound: both branches of an if/else are counted.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src" / "activities" / "settings" / "SettingsActivity.cpp"

BUILDER = re.compile(r"(?m)^std::vector<SettingInfo> (build\w+Settings)\([^)]*\) \{")
RESERVE = re.compile(r"\bv\.reserve\((\d+)\)")
PUSH = re.compile(r"\bv\.(?:push_back|emplace_back)\(")


def builders(text):
    """Yields (name, body) for each top-level build*Settings() function."""
    for match in BUILDER.finditer(text):
        depth, index = 1, match.end()
        while depth and index < len(text):
            depth += text[index] == "{"
            depth -= text[index] == "}"
            index += 1
        yield match.group(1), text[match.end():index]


class SettingsReserveTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.builders = dict(builders(SOURCE.read_text(encoding="utf-8")))

    def test_builders_are_found(self):
        self.assertIn("buildAppSettings", self.builders)

    def test_app_settings_reserves_its_rows(self):
        # The Apps tab is the largest list; it must keep an explicit reserve.
        self.assertRegex(self.builders["buildAppSettings"], RESERVE)

    def test_rows_never_exceed_reserve(self):
        for name, body in self.builders.items():
            reserve = RESERVE.search(body)
            if not reserve:
                continue
            rows = len(PUSH.findall(body))
            self.assertLessEqual(
                rows,
                int(reserve.group(1)),
                f"{name} pushes up to {rows} rows but reserves {reserve.group(1)}; "
                "raise the reserve or the list reallocates and can throw bad_alloc on device",
            )


if __name__ == "__main__":
    unittest.main()

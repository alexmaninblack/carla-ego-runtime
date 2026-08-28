import re
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
CYRILLIC = re.compile(r"[\u0400-\u04ff]")
PRODUCT_SUFFIXES = {
    ".c",
    ".cc",
    ".cmake",
    ".command",
    ".cpp",
    ".h",
    ".hpp",
    ".ini",
    ".json",
    ".md",
    ".plist",
    ".py",
    ".sh",
    ".swift",
    ".toml",
    ".txt",
    ".xml",
    ".yaml",
    ".yml",
}


class ProductLanguageTests(unittest.TestCase):
    def test_repository_product_text_is_english_only(self):
        violations = []
        for path in REPOSITORY.rglob("*"):
            if ".git" in path.parts or not path.is_file():
                continue
            if path.suffix not in PRODUCT_SUFFIXES and path.name != "CMakeLists.txt":
                continue
            text = path.read_text(encoding="utf-8", errors="ignore")
            for line_number, line in enumerate(text.splitlines(), start=1):
                if CYRILLIC.search(line):
                    violations.append(f"{path.relative_to(REPOSITORY)}:{line_number}")
        self.assertEqual(violations, [], "Cyrillic product text found")

    def test_wheel_angular_speed_label_uses_vss_units(self):
        source = (REPOSITORY / "src" / "viss_client.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("Angular deg/s", source)
        self.assertNotIn("Angular rad/s", source)


if __name__ == "__main__":
    unittest.main()

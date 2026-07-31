import importlib.util
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "format_c_comments.py"
SPEC = importlib.util.spec_from_file_location("format_c_comments", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
FORMATTER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FORMATTER)


class FormatCCommentsTests(unittest.TestCase):
    def run_formatter(self, content: bytes, mode: int = 0o640):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "sample.c"
        path.write_bytes(content)
        path.chmod(mode)
        result = subprocess.run(
            [sys.executable, str(SCRIPT), str(path)],
            capture_output=True,
            text=True,
            check=False,
        )
        return path, result

    def test_formats_tags_and_aligns_wrapped_text(self):
        source = b"""/**
 * @brief This sentence has enough repeated words to require wrapping while keeping every resulting physical line within the configured width limit.
 * @param context This is a parameter description that also has enough repeated words to require a properly aligned continuation line.
 * @param[in] mode Selects the requested mode.
 * @param [out] result Receives the result.
 * @return This return description is intentionally long enough to require wrapping onto another aligned line.
 */
int f(void);
"""
        path, result = self.run_formatter(source)
        self.assertEqual(result.returncode, 0, result.stderr)
        output = path.read_text()
        expected_content = [
            "This sentence",
            "every resulting",
            "This is a parameter",
            "require a properly",
            "Selects the requested",
            "Receives the result",
            "This return",
            "another aligned",
        ]
        content_columns = {
            line.index(content)
            for line in output.splitlines()
            for content in expected_content
            if content in line
        }
        self.assertEqual(content_columns, {23})
        for line in output.splitlines():
            self.assertLessEqual(FORMATTER.display_width(line), 100)

    def test_wraps_chinese_by_display_width(self):
        paragraph = "中文说明" * 30
        source = f"/**\n * @brief {paragraph}\n */\n".encode()
        path, result = self.run_formatter(source)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = path.read_text().splitlines()
        self.assertGreater(len(lines), 3)
        self.assertTrue(all(FORMATTER.display_width(line) <= 100 for line in lines))
        self.assertTrue(all(line.startswith(" *        ") for line in lines[2:-1]))

    def test_uses_one_content_column_per_documentation_comment(self):
        source = b"""/**
 * @brief summary
 * @param very_long_parameter value
 * @return result
 */
/**
 * @brief another summary
 * @return another result
 */
"""
        path, result = self.run_formatter(source)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = path.read_text().splitlines()
        first_columns = {
            lines[1].index("summary"),
            lines[2].index("value"),
            lines[3].index("result"),
        }
        second_columns = {
            lines[6].index("another summary"),
            lines[7].index("another result"),
        }
        self.assertEqual(len(first_columns), 1)
        self.assertEqual(len(second_columns), 1)
        self.assertNotEqual(first_columns, second_columns)

    def test_does_not_insert_spaces_at_chinese_source_line_breaks(self):
        source = """/**
 * @return 参数无效、缓冲区不足或棋盘字段中包含
 *         无法识别的字母时返回 false。
 */
""".encode()
        path, result = self.run_formatter(source)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("包含无法", path.read_text())
        self.assertNotIn("包含 无法", path.read_text())

    def test_stops_at_blank_line_and_next_command(self):
        source = b"""/**
 * @brief short text
 *
 * This ordinary paragraph must remain exactly as written.
 * @note This command is not formatted even when it contains a very long line that exceeds the configured limit by a lot.
 * @return a value
 */
"""
        path, result = self.run_formatter(source)
        self.assertEqual(result.returncode, 0, result.stderr)
        output = path.read_text()
        self.assertIn(" * @brief  short text", output)
        self.assertIn(" * This ordinary paragraph must remain exactly as written.", output)
        self.assertIn(" * @note This command is not formatted", output)
        self.assertIn(" * @return a value", output)

    def test_ignores_non_documentation_comments_and_literals(self):
        source = b'''const char *s = "/** @brief not a comment */";
// /** @return also not a comment */
/* @brief ordinary block comment */
/**
 * @brief real documentation
 */
'''
        path, result = self.run_formatter(source)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(path.read_bytes(), source)

    def test_ignores_documentation_marker_in_spliced_line_comment(self):
        source = (
            "// The next physical line is still part of this comment. \\\n"
            "/** @brief " + "word " * 30 + "*/\n"
        ).encode()
        path, result = self.run_formatter(source)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(path.read_bytes(), source)

    def test_expands_an_overlong_single_line_documentation_comment(self):
        source = ("/** @return " + "word " * 30 + "*/\n").encode()
        path, result = self.run_formatter(source)
        self.assertEqual(result.returncode, 0, result.stderr)
        output = path.read_text()
        self.assertTrue(output.startswith("/**\n * @return "))
        self.assertTrue(output.endswith("\n */\n"))
        self.assertTrue(
            all(FORMATTER.display_width(line) <= 100 for line in output.splitlines())
        )

    def test_preserves_protected_line_breaks_and_aligns_them(self):
        source = b"""/**
 * @brief Items:
 * - first item
 * - second item
 */
"""
        path, result = self.run_formatter(source)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            path.read_text(),
            "/**\n * @brief Items:\n *        - first item\n *        - second item\n */\n",
        )

    def test_warns_for_an_unbreakable_token_but_formats_other_text(self):
        token = "https://example.test/" + "x" * 110
        source = f"/**\n * @brief before {token}\n * @return a value\n */\n".encode()
        path, result = self.run_formatter(source)
        self.assertEqual(result.returncode, 1)
        self.assertIn("cannot be safely wrapped", result.stderr)
        self.assertIn(token, path.read_text())

    def test_does_not_split_a_url_containing_wide_characters(self):
        token = "https://example.test/" + "中" * 60
        source = f"/**\n * @brief {token}\n */\n".encode()
        path, result = self.run_formatter(source)
        self.assertEqual(result.returncode, 1)
        self.assertIn(token, path.read_text())

    def test_preserves_crlf_missing_final_newline_and_permissions(self):
        source = (
            "/**\r\n * @return "
            + "word " * 30
            + "\r\n */\r\nint value;"
        ).encode()
        path, result = self.run_formatter(source, 0o604)
        self.assertEqual(result.returncode, 0, result.stderr)
        output = path.read_bytes()
        self.assertNotIn(b"\n", output.replace(b"\r\n", b""))
        self.assertFalse(output.endswith((b"\r", b"\n")))
        self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o604)

    def test_is_idempotent(self):
        source = ("/**\n * @brief " + "word " * 40 + "\n */\n").encode()
        path, first = self.run_formatter(source)
        self.assertEqual(first.returncode, 0, first.stderr)
        once = path.read_bytes()
        second = subprocess.run(
            [sys.executable, str(SCRIPT), str(path)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(path.read_bytes(), once)

    def test_rejects_non_c_file_and_invalid_utf8(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        text_path = Path(directory.name) / "sample.h"
        text_path.write_text("/** @brief no */")
        wrong_suffix = subprocess.run(
            [sys.executable, str(SCRIPT), str(text_path)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(wrong_suffix.returncode, 2)

        c_path = Path(directory.name) / "bad.c"
        c_path.write_bytes(b"\xff")
        invalid_utf8 = subprocess.run(
            [sys.executable, str(SCRIPT), str(c_path)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(invalid_utf8.returncode, 2)
        self.assertEqual(c_path.read_bytes(), b"\xff")

        target_path = Path(directory.name) / "target.c"
        target_path.write_text("/** @brief unchanged */")
        link_path = Path(directory.name) / "link.c"
        link_path.symlink_to(target_path)
        symlink = subprocess.run(
            [sys.executable, str(SCRIPT), str(link_path)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(symlink.returncode, 2)
        self.assertEqual(target_path.read_text(), "/** @brief unchanged */")


if __name__ == "__main__":
    unittest.main()

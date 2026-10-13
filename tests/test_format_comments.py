"""Regression checks for paragraph reflow and the editor replacement protocol."""

from pathlib import Path
import subprocess
import sys
import unittest
import xml.etree.ElementTree as ET

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
from format_c_comments import format_source
from clang_format_with_comments import apply_replacements, find_clang_format


SPLIT = """/**
 * These tables describe location only;
 * they do not account for attacks, blocked horse legs, cannon screens or game phase.
 */
void f(void) { foo(first_argument, second_argument); }
"""
JOINED = SPLIT.replace("only;\n * they", "only; they")


class ParagraphTests(unittest.TestCase):
    def test_original_breaks_do_not_change_result(self):
        for marker in ("", "@brief ", "@param pos ", "@return ", "@retval true "):
            split = SPLIT.replace("These tables", marker + "These tables")
            joined = JOINED.replace("These tables", marker + "These tables")
            with self.subTest(marker=marker):
                result, warnings = format_source(split)
                self.assertEqual(result, format_source(joined)[0])
                self.assertEqual(result, format_source(result)[0])
                self.assertFalse(warnings)
                self.assertTrue(all(len(line) <= 100 for line in result.splitlines()))

    def test_plain_block_comment(self):
        self.assertEqual(
            format_source(SPLIT.replace("/**", "/*"))[0],
            format_source(JOINED.replace("/**", "/*"))[0],
        )

    def test_prose_on_opening_and_closing_lines(self):
        split = SPLIT.replace("/**\n * These", "/* These").replace("phase.\n */", "phase. */")
        joined = JOINED.replace("/**\n * These", "/* These").replace("phase.\n */", "phase. */")
        result = format_source(split)[0]
        self.assertEqual(result, format_source(joined)[0])
        self.assertEqual(result, format_source(result)[0])
        self.assertEqual(format_source("/* short comment */")[0], "/* short comment */")

    def test_blank_line_separates_paragraphs(self):
        source = "/**\n * First paragraph.\n *\n * Second paragraph.\n */\n"
        self.assertEqual(format_source(source)[0], source)

    def test_structured_content_is_preserved(self):
        for body in (
            " * - first item\n *   continuation\n * - second item",
            " * @param pos - first item\n *   continuation\n * - second item",
            " * | Column | Value |\n * | ------ | ----- |",
            " *     if (condition)\n *         foo();",
            " * @code\n * foo();\n * @endcode",
            " * ```c\n * foo();\n * ```",
            " * @verbatim\n * a    b\n * @endverbatim",
        ):
            source = "/**\n" + body + "\n */\n"
            with self.subTest(body=body):
                self.assertEqual(format_source(source)[0], source)

    def test_strings_and_disabled_regions_are_preserved(self):
        for off, on in (("// clang-format off", "// clang-format on"),
                        ("/* clang-format off */", "/* clang-format on */")):
            source = off + "\n" + SPLIT + on + "\n" + SPLIT
            result = format_source(source)[0]
            self.assertTrue(result.startswith(off + "\n" + SPLIT + on))
            self.assertTrue(result.endswith(format_source(SPLIT)[0]))
        source = 'const char *s = "/** not a comment */";\n'
        self.assertEqual(format_source(source)[0], source)

    def test_inline_comments_do_not_split_code_or_macros(self):
        comment = "/* " + "a long comment " * 20 + "*/"
        for source in ("#define VALUE " + comment + " 1\n", "int x; " + comment + "\n"):
            self.assertEqual(format_source(source)[0], source)

    def test_unicode_and_crlf(self):
        source = "/**\r\n * 中文注释说明。\r\n * 继续说明。\r\n */\r\n"
        result = format_source(source)[0]
        self.assertIn("中文注释说明。继续说明。", result)
        self.assertNotIn("\n", result.replace("\r\n", ""))


class EditorAdapterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.binary = find_clang_format()
        except OSError as error:
            raise unittest.SkipTest(str(error))

    def format(self, source, *arguments):
        result = subprocess.run([
            sys.executable, str(SCRIPTS / "clang_format_with_comments.py"),
            "--style=file", "--assume-filename=" + str(SCRIPTS.parent / "src/engine.c"),
            "--output-replacements-xml", *arguments,
        ], input=source.encode(), capture_output=True, check=True)
        return apply_replacements(source.encode(), ET.fromstring(result.stdout)).decode()

    def test_whole_document_and_repeat_formatting(self):
        result = self.format(SPLIT)
        self.assertEqual(result, self.format(JOINED))
        self.assertEqual(result, self.format(result))
        self.assertIn("void f(void)\n{\n    foo(", result)

    def test_full_byte_range_and_unicode_offsets(self):
        source = "// 中文前缀\n" + SPLIT
        result = self.format(source, "--offset=0", f"--length={len(source.encode())}")
        self.assertEqual(result, self.format(source))
        self.assertEqual(result, self.format(result))

    def test_xml_preserves_crlf(self):
        source = SPLIT.replace("\n", "\r\n")
        result = self.format(source)
        self.assertNotIn("\n", result.replace("\r\n", ""))
        self.assertEqual(result, self.format(result))

    def test_selection_does_not_reflow_other_comments(self):
        source = SPLIT
        offset = source.index("void")
        args = [f"--offset={offset}", f"--length={len(source) - offset}"]
        actual = self.format(source, *args)
        expected = subprocess.run([
            self.binary, "--style=file", "--assume-filename=src/engine.c", *args,
        ], input=source, text=True, capture_output=True, check=True).stdout
        self.assertEqual(actual, expected)

    def test_version_is_forwarded(self):
        result = subprocess.run([str(SCRIPTS / "clang_format_with_comments.py"), "--version"],
                                capture_output=True, text=True, check=True)
        self.assertIn("clang-format version", result.stdout)


if __name__ == "__main__":
    unittest.main()

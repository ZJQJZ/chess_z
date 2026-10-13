#!/usr/bin/env python3
"""clang-format adapter for VS Code's XML replacement protocol.

Whole-document formatting also reflows block-comment paragraphs. Range formatting
and command-line operations are delegated to clang-format without extra edits.
"""

from __future__ import annotations

import difflib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET

from format_c_comments import FormatError, format_source


def find_clang_format() -> str:
    override = os.environ.get("CHESS_CLANG_FORMAT_BINARY")
    if override:
        return override
    # Use the same bundled LLVM as the installed C/C++ extension. Version sorting
    # avoids pinning this workspace to one extension release.
    bundled = []
    for root in (".vscode", ".vscode-insiders"):
        bundled.extend((Path.home() / root / "extensions").glob(
            "ms-vscode.cpptools-*/LLVM/bin/clang-format"
        ))
    bundled.sort(key=lambda p: tuple(
        int(n) for n in re.findall(r"\d+", p.parents[2].name)
    ), reverse=True)
    for candidate in [*map(str, bundled), shutil.which("clang-format")]:
        if candidate and Path(candidate).resolve() != Path(__file__).resolve():
            if os.access(candidate, os.X_OK):
                return candidate
    raise OSError("clang-format not found; install the C/C++ extension or set CHESS_CLANG_FORMAT_BINARY")


def option_values(arguments: list[str], name: str) -> list[str]:
    values = []
    for index, argument in enumerate(arguments):
        if argument.lstrip("-").startswith(name + "="):
            values.append(argument.split("=", 1)[1])
        elif argument.lstrip("-") == name and index + 1 < len(arguments):
            values.append(arguments[index + 1])
    return values


def is_whole_document(arguments: list[str], source: bytes) -> bool:
    offsets = option_values(arguments, "offset")
    lengths = option_values(arguments, "length")
    lines = option_values(arguments, "lines")
    if lines:
        return len(lines) == 1 and lines[0] == f"1:{len(source.splitlines())}"
    return (not offsets and not lengths) or (
        offsets == ["0"] and (not lengths or lengths == [str(len(source))])
    )


def apply_replacements(source: bytes, root: ET.Element) -> bytes:
    for replacement in sorted(
        root.findall("replacement"), key=lambda r: int(r.attrib["offset"]), reverse=True
    ):
        offset = int(replacement.attrib["offset"])
        length = int(replacement.attrib["length"])
        source = source[:offset] + (replacement.text or "").encode("utf-8") + source[offset + length:]
    return source


def replacement_xml(original: bytes, formatted: bytes, root: ET.Element) -> bytes:
    # Diff complete UTF-8 lines so byte offsets cannot split a multibyte character.
    before, after = original.splitlines(keepends=True), formatted.splitlines(keepends=True)
    offsets = [0]
    for line in before:
        offsets.append(offsets[-1] + len(line))
    for replacement in root.findall("replacement"):
        root.remove(replacement)
    for kind, first, last, new_first, new_last in difflib.SequenceMatcher(
        a=before, b=after, autojunk=False
    ).get_opcodes():
        if kind != "equal":
            replacement = ET.SubElement(root, "replacement", {
                "offset": str(offsets[first]),
                "length": str(offsets[last] - offsets[first]),
            })
            replacement.text = b"".join(after[new_first:new_last]).decode("utf-8")
    # XML normalizes literal CR/CRLF in text nodes; entities preserve file endings.
    return ET.tostring(root, encoding="utf-8", xml_declaration=True).replace(b"\r", b"&#13;")


def main(arguments: list[str] | None = None) -> int:
    arguments = sys.argv[1:] if arguments is None else arguments
    try:
        binary = find_clang_format()
        xml_output = any(a.lstrip("-") == "output-replacements-xml" for a in arguments)
        filenames = option_values(arguments, "assume-filename")
        if not xml_output or not filenames or Path(filenames[-1]).suffix not in {".c", ".h"}:
            return subprocess.run([binary, *arguments]).returncode

        source = sys.stdin.buffer.read()
        result = subprocess.run([binary, *arguments], input=source, capture_output=True)
        sys.stderr.buffer.write(result.stderr)
        output = result.stdout
        if result.returncode == 0 and is_whole_document(arguments, source):
            root = ET.fromstring(output)
            if root.attrib.get("incomplete_format") != "true":
                formatted = apply_replacements(source, root).decode("utf-8")
                reflowed, warnings = format_source(formatted)
                output = replacement_xml(source, reflowed.encode("utf-8"), root)
                for line, token in warnings:
                    print(f"warning: comment line {line}: cannot wrap token: {token}", file=sys.stderr)
        sys.stdout.buffer.write(output)
        return result.returncode
    except (FormatError, OSError, UnicodeError, ET.ParseError, ValueError) as error:
        print(f"error: comment formatter: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

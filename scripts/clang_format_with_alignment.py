#!/usr/bin/env python3
"""Run clang-format, then align initializers across plain C declarations.

The extra pass is restricted to C/header source read from stdin with an
--assume-filename argument. File operations and informational commands retain
clang-format's own behavior. The separate comment-reflow adapter is not used.
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

from align_c_declarations import align_declaration_initializers
from clang_format_with_comments import apply_replacements, option_values, replacement_xml


def find_clang_format() -> str:
    """Find an actual formatter rather than another copy of this adapter."""
    wrappers = {
        Path(__file__).resolve(),
        Path(__file__).with_name("clang_format_with_comments.py").resolve(),
    }
    override = os.environ.get("CHESS_CLANG_FORMAT_BINARY")
    if override:
        candidate = shutil.which(override) or override
        if Path(candidate).resolve() in wrappers:
            raise OSError("CHESS_CLANG_FORMAT_BINARY must name clang-format, not an adapter")
        if not os.access(candidate, os.X_OK):
            raise OSError(f"clang-format is not executable: {candidate}")
        return candidate

    bundled: list[Path] = []
    for root in (".vscode", ".vscode-insiders"):
        bundled.extend((Path.home() / root / "extensions").glob(
            "ms-vscode.cpptools-*/LLVM/bin/clang-format"
        ))
    bundled.sort(key=lambda p: tuple(
        int(n) for n in re.findall(r"\d+", p.parents[2].name)
    ), reverse=True)
    for candidate in [*map(str, bundled), shutil.which("clang-format")]:
        if candidate and Path(candidate).resolve() not in wrappers and os.access(candidate, os.X_OK):
            return candidate
    raise OSError("clang-format not found; install the C/C++ extension or set CHESS_CLANG_FORMAT_BINARY")


def option_names(arguments: list[str]) -> set[str]:
    return {argument.lstrip("-").split("=", 1)[0]
            for argument in arguments if argument.startswith("-")}


def can_postprocess(arguments: list[str]) -> bool:
    """Exclude files, response files, and commands that do not format stdin."""
    names = option_names(arguments)
    if names & {"version", "help", "help-hidden", "help-list", "help-list-hidden",
                "dump-config", "dry-run", "n", "i", "files"}:
        return False
    if "cursor" in names and "output-replacements-xml" not in names:
        # Text-mode --cursor has a JSON preamble; keep that CLI protocol intact.
        return False
    value_options = {"assume-filename", "cursor", "fallback-style", "ferror-limit",
                     "length", "lines", "offset", "qualifier-alignment", "style", "Wno-error"}
    skip_value = False
    for argument in arguments:
        if skip_value:
            skip_value = False
            continue
        if argument == "-":
            continue
        if not argument.startswith("-"):
            return False
        if "=" not in argument and argument.lstrip("-") in value_options:
            skip_value = True
    filenames = option_values(arguments, "assume-filename")
    return bool(filenames and Path(filenames[-1]).suffix in {".c", ".h"})


def effective_alignment_settings(binary: str, arguments: list[str]) -> tuple[bool, int, int]:
    """Read clang-format's resolved configuration without forwarding file edits."""
    config_arguments = ["--dump-config"]
    for name in ("assume-filename", "style", "fallback-style"):
        values = option_values(arguments, name)
        if values:
            config_arguments.append(f"--{name}={values[-1]}")
    result = subprocess.run([binary, *config_arguments], input=b"", capture_output=True)
    if result.returncode:
        raise OSError(result.stderr.decode("utf-8", errors="replace").strip()
                      or "clang-format --dump-config failed")
    config = result.stdout.decode("utf-8")

    def scalar(name: str, default: str) -> str:
        match = re.search(rf"^{name}:[ \t]*([^\n\r]*)", config, re.MULTILINE)
        return match.group(1).strip() if match else default

    alignment = scalar("AlignConsecutiveAssignments", "None")
    if alignment:
        enabled = alignment.lower() not in {"none", "false"}
    else:
        block = re.search(r"^AlignConsecutiveAssignments:[ \t]*\r?\n((?:[ \t]+[^\n]*\n?)*)",
                          config, re.MULTILINE)
        enabled = bool(block and re.search(r"^\s+Enabled:\s*true\s*$", block.group(1), re.MULTILINE))
    enabled = enabled and scalar("DisableFormat", "false").lower() != "true"
    return enabled, int(scalar("ColumnLimit", "100")), int(scalar("TabWidth", "8"))


def selected_byte_ranges(arguments: list[str], source: bytes) -> list[tuple[int, int]] | None:
    """Return half-open source ranges, or None when the entire file was selected."""
    lines = source.splitlines(keepends=True)
    line_offsets = [0]
    for line in lines:
        line_offsets.append(line_offsets[-1] + len(line))
    ranges: list[tuple[int, int]] = []
    line_arguments = option_values(arguments, "lines")
    if line_arguments:
        for value in line_arguments:
            first, last = map(int, value.split(":"))
            first = min(max(first - 1, 0), len(lines))
            last = min(max(last, 0), len(lines))
            ranges.append((line_offsets[first], line_offsets[last]))
    else:
        offsets = option_values(arguments, "offset")
        lengths = option_values(arguments, "length")
        if not offsets and not lengths:
            return None
        for index, value in enumerate(offsets):
            start = int(value)
            end = start + int(lengths[index]) if index < len(lengths) else len(source)
            ranges.append((max(0, start), min(len(source), end)))
    merged: list[tuple[int, int]] = []
    for start, end in sorted(ranges):
        if end <= start:
            continue
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    return None if merged == [(0, len(source))] else merged


def mapped_line_ranges(arguments: list[str], source: bytes, formatted: bytes,
                       root: ET.Element) -> list[tuple[int, int]] | None:
    """Map selections through clang edits, retaining only fully selected lines."""
    ranges = selected_byte_ranges(arguments, source)
    if ranges is None:
        return None
    edits = sorted((int(r.attrib["offset"]), int(r.attrib["length"]),
                    len((r.text or "").encode("utf-8"))) for r in root.findall("replacement"))

    def map_boundary(position: int, *, start: bool) -> int:
        delta = 0
        for offset, length, new_length in edits:
            if position < offset:
                break
            if position == offset:
                # A boundary insertion could belong to the unselected side.
                return offset + delta + (new_length if start and length == 0 else 0)
            if position < offset + length:
                # If a clang edit crosses the boundary, do not grow the selection.
                return offset + delta + (new_length if start else 0)
            delta += new_length - length
        return position + delta

    mapped = [(map_boundary(start, start=True), map_boundary(end, start=False))
              for start, end in ranges]
    selected: list[int] = []
    position = 0
    for number, line in enumerate(formatted.splitlines(keepends=True), 1):
        content_end = position + len(line.rstrip(b"\r\n"))
        if any(start <= position and content_end <= end and start < end for start, end in mapped):
            selected.append(number)
        position += len(line)
    line_ranges: list[tuple[int, int]] = []
    for number in selected:
        if line_ranges and number == line_ranges[-1][1] + 1:
            line_ranges[-1] = (line_ranges[-1][0], number)
        else:
            line_ranges.append((number, number))
    return line_ranges


def update_cursor(root: ET.Element, before: bytes, after: bytes) -> None:
    cursor = root.find("cursor")
    if cursor is None or cursor.text is None or before == after:
        return
    position = int(cursor.text)
    # The alignment pass preserves line count. Diff only the cursor's line;
    # comparing a whole C file byte-by-byte is costly on repeated whitespace.
    before_offset = after_offset = 0
    before_line = after_line = b""
    for before_line, after_line in zip(before.splitlines(keepends=True), after.splitlines(keepends=True)):
        if position <= before_offset + len(before_line):
            break
        before_offset += len(before_line)
        after_offset += len(after_line)
    position -= before_offset
    delta = 0
    for kind, first, last, new_first, new_last in difflib.SequenceMatcher(
        a=before_line, b=after_line, autojunk=False
    ).get_opcodes():
        if kind == "equal":
            continue
        if position < first:
            break
        if position <= last:
            cursor.text = str(after_offset + new_first + min(position - first, new_last - new_first))
            return
        delta += (new_last - new_first) - (last - first)
    cursor.text = str(after_offset + position + delta)


def main(arguments: list[str] | None = None) -> int:
    arguments = sys.argv[1:] if arguments is None else arguments
    try:
        binary = find_clang_format()
        if not can_postprocess(arguments):
            return subprocess.run([binary, *arguments]).returncode
        xml_output = "output-replacements-xml" in option_names(arguments)
        source = sys.stdin.buffer.read()
        clang_arguments = arguments if xml_output else [*arguments, "--output-replacements-xml"]
        result = subprocess.run([binary, *clang_arguments], input=source, capture_output=True)
        sys.stderr.buffer.write(result.stderr)
        if result.returncode:
            sys.stdout.buffer.write(result.stdout)
            return result.returncode
        root = ET.fromstring(result.stdout)
        formatted = apply_replacements(source, root)
        aligned = formatted
        if root.attrib.get("incomplete_format") != "true":
            enabled, column_limit, tab_width = effective_alignment_settings(binary, arguments)
            if enabled:
                aligned = align_declaration_initializers(
                    formatted.decode("utf-8"),
                    line_ranges=mapped_line_ranges(arguments, source, formatted, root),
                    column_limit=column_limit,
                    tab_width=tab_width,
                ).encode("utf-8")
        if xml_output:
            if aligned == formatted:
                output = result.stdout
            else:
                update_cursor(root, formatted, aligned)
                output = replacement_xml(source, aligned, root)
        else:
            output = aligned
        sys.stdout.buffer.write(output)
        return 0
    except (OSError, UnicodeError, ET.ParseError, ValueError) as error:
        print(f"error: declaration alignment formatter: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

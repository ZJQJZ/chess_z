#!/usr/bin/env python3
"""Format selected Doxygen paragraphs in one C source file."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import stat
import sys
import tempfile
import unicodedata


COLUMN_LIMIT = 100
TAB_SIZE = 8

_TAG_RE = re.compile(
    r"^(?P<leader>[ \t]*\*[ \t]*)(?P<tag>"
    r"@brief\b|@return\b|"
    r"@param(?:[ \t]*\[(?P<param_direction>[^\]\r\n]+)\])?"
    r"[ \t]+(?P<param_name>\S+)"
    r")(?P<separator>[ \t]*)(?P<body>.*)$"
)
_COMMENT_LINE_RE = re.compile(r"^[ \t]*\*(?P<after>.*)$")
_LIST_RE = re.compile(r"(?:[-+*]|\d+[.)])[ \t]+")
_FENCE_RE = re.compile(r"(?:```|~~~)")
_URL_RE = re.compile(r"(?:https?|ftp)://|www\.", re.IGNORECASE)
_SINGLE_LINE_DOC_RE = re.compile(
    r"^(?P<indent>[ \t]*)/\*\*[ \t]*(?P<content>@(?:brief|return|param)\b.*?)"
    r"[ \t]*\*/$"
)


class FormatError(Exception):
    """Raised when an input file cannot be safely formatted."""


def display_width(text: str, start_column: int = 0) -> int:
    """Return the number of terminal columns occupied by *text*."""
    column = start_column
    for character in text:
        if character == "\t":
            column += TAB_SIZE - column % TAB_SIZE
        elif unicodedata.combining(character) or unicodedata.category(character) in {
            "Mn",
            "Me",
        }:
            continue
        elif unicodedata.east_asian_width(character) in {"W", "F"}:
            column += 2
        else:
            column += 1
    return column - start_column


def _line_parts(line: str) -> tuple[str, str]:
    if line.endswith("\r\n"):
        return line[:-2], "\r\n"
    if line.endswith(("\n", "\r")):
        return line[:-1], line[-1:]
    return line, ""


def _is_breakable_character(character: str) -> bool:
    return unicodedata.east_asian_width(character) in {"W", "F"}


def _split_mixed_token(token: str) -> list[str]:
    """Split a token at wide-character boundaries, preserving narrow words."""
    if _URL_RE.match(token) or not any(
        _is_breakable_character(character) for character in token
    ):
        return [token]

    pieces: list[str] = []
    narrow: list[str] = []
    for character in token:
        if _is_breakable_character(character):
            if narrow:
                pieces.append("".join(narrow))
                narrow.clear()
            pieces.append(character)
        elif unicodedata.combining(character) and pieces and not narrow:
            pieces[-1] += character
        else:
            narrow.append(character)
    if narrow:
        pieces.append("".join(narrow))
    return pieces


def _paragraph_units(text: str) -> list[tuple[str, bool]]:
    units: list[tuple[str, bool]] = []
    for token_index, token in enumerate(re.findall(r"\S+", text)):
        for piece_index, piece in enumerate(_split_mixed_token(token)):
            units.append((piece, token_index > 0 and piece_index == 0))
    return units


def _wrap_paragraph(
    text: str, capacity: int
) -> tuple[list[str], list[str]]:
    if capacity <= 0:
        return [text], [text] if text else []

    output: list[str] = []
    current = ""
    warnings: list[str] = []
    for unit, separated in _paragraph_units(text):
        separator = " " if separated and current else ""
        candidate = current + separator + unit
        if current and display_width(candidate) > capacity:
            output.append(current)
            current = unit
        else:
            current = candidate

        if display_width(unit) > capacity and unit not in warnings:
            warnings.append(unit)

    if current:
        output.append(current)
    return output, warnings


def _is_protected(lines: list[str]) -> bool:
    for line in lines:
        stripped = line.lstrip()
        if _LIST_RE.match(stripped) or _FENCE_RE.match(stripped):
            return True
    return False


def _join_paragraph_lines(lines: list[str]) -> str:
    paragraph = ""
    for line in lines:
        if not line:
            continue
        if paragraph and not (
            _is_breakable_character(paragraph[-1])
            and _is_breakable_character(line[0])
        ):
            paragraph += " "
        paragraph += line
    return paragraph


def _render_lines(
    content: list[str], endings: list[str], fallback_newline: str
) -> str:
    rendered: list[str] = []
    for index, line in enumerate(content):
        if index == len(content) - 1:
            ending = endings[-1]
        else:
            ending = fallback_newline
        rendered.append(line + ending)
    return "".join(rendered)


def _format_doc_comment(
    comment: str, start_line: int, newline: str
) -> tuple[str, list[tuple[int, str]]]:
    single_line = _SINGLE_LINE_DOC_RE.match(comment)
    if single_line is not None and display_width(comment) > COLUMN_LIMIT:
        indent = single_line.group("indent")
        expanded = (
            f"{indent}/**{newline}"
            f"{indent} * {single_line.group('content')}{newline}"
            f"{indent} */"
        )
        return _format_doc_comment(expanded, start_line, newline)

    source_lines = comment.splitlines(keepends=True)
    plain_lines: list[str] = []
    endings: list[str] = []
    for line in source_lines:
        plain, ending = _line_parts(line)
        plain_lines.append(plain)
        endings.append(ending)

    tag_matches = [
        match
        for line in plain_lines
        if (match := _TAG_RE.match(line)) is not None
    ]
    parameter_name_column = max(
        (
            display_width(
                match.group("leader")
                + "@param"
                + (
                    f"[{match.group('param_direction')}]"
                    if match.group("param_direction") is not None
                    else ""
                )
            )
            + 1
            for match in tag_matches
            if match.group("param_name") is not None
        ),
        default=0,
    )

    def render_tag_prefix(match: re.Match[str]) -> str:
        leader = match.group("leader")
        parameter_name = match.group("param_name")
        if parameter_name is None:
            return leader + match.group("tag")

        direction = match.group("param_direction")
        parameter_prefix = leader + "@param"
        if direction is not None:
            parameter_prefix += f"[{direction}]"
        return (
            parameter_prefix
            + " " * (parameter_name_column - display_width(parameter_prefix))
            + parameter_name
        )

    structured_content_column = max(
        (
            display_width(render_tag_prefix(match)) + 1
            for match in tag_matches
            if match.group("tag") != "@brief"
        ),
        default=0,
    )

    warnings: list[tuple[int, str]] = []
    output: list[str] = []
    index = 0
    while index < len(plain_lines):
        match = _TAG_RE.match(plain_lines[index])
        if match is None:
            output.append(source_lines[index])
            index += 1
            continue

        body = match.group("body").strip()
        paragraph_lines = [body] if body else []
        consumed_end = index + 1
        while consumed_end < len(plain_lines):
            if re.match(r"^[ \t]*\*/", plain_lines[consumed_end]):
                break
            continuation = _COMMENT_LINE_RE.match(plain_lines[consumed_end])
            if continuation is None:
                break
            after_star = continuation.group("after")
            stripped = after_star.strip()
            if not stripped or stripped.startswith("@"):
                break
            paragraph_lines.append(stripped)
            consumed_end += 1

        if not paragraph_lines:
            output.append(source_lines[index])
            index += 1
            continue

        leader = match.group("leader")
        tag = match.group("tag")
        tag_prefix = render_tag_prefix(match)
        content_column = (
            display_width(tag_prefix) + 1
            if tag == "@brief"
            else structured_content_column
        )
        first_prefix = tag_prefix + " " * (
            content_column - display_width(tag_prefix)
        )
        star_prefix = leader[: leader.rfind("*") + 1]
        continuation_prefix = star_prefix + " " * (
            content_column - display_width(star_prefix)
        )
        capacity = COLUMN_LIMIT - content_column

        formatted_content: list[str]
        warning_tokens: list[str] = []
        if _is_protected(paragraph_lines):
            formatted_content = paragraph_lines
            for line in formatted_content:
                if display_width(continuation_prefix + line) > COLUMN_LIMIT:
                    warning_tokens.append(line)
        else:
            paragraph = _join_paragraph_lines(paragraph_lines)
            formatted_content, warning_tokens = _wrap_paragraph(paragraph, capacity)

        rendered_content = [first_prefix + formatted_content[0]]
        rendered_content.extend(continuation_prefix + line for line in formatted_content[1:])
        consumed_endings = endings[index:consumed_end]
        output.append(_render_lines(rendered_content, consumed_endings, newline))

        for token in warning_tokens:
            preview = token if len(token) <= 48 else token[:45] + "..."
            warnings.append((start_line + index, preview))
        index = consumed_end

    return "".join(output), warnings


def _find_doc_comments(source: str) -> list[tuple[int, int]]:
    ranges: list[tuple[int, int]] = []
    index = 0
    state = "normal"
    while index < len(source):
        character = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""

        if state == "normal":
            if character == '"':
                state = "string"
                index += 1
            elif character == "'":
                state = "character"
                index += 1
            elif character == "/" and following == "/":
                state = "line_comment"
                index += 2
            elif character == "/" and following == "*":
                is_documentation = index + 2 < len(source) and source[index + 2] == "*"
                end = source.find("*/", index + 2)
                if end == -1:
                    line_number = len(re.findall(r"\r\n|\n|\r", source[:index])) + 1
                    raise FormatError(
                        f"unterminated block comment at line {line_number}"
                    )
                if is_documentation:
                    ranges.append((index, end + 2))
                index = end + 2
            else:
                index += 1
        elif state in {"string", "character"}:
            if character == "\\":
                if source.startswith("\r\n", index + 1):
                    index += 3
                else:
                    index += 2
            elif (state == "string" and character == '"') or (
                state == "character" and character == "'"
            ):
                state = "normal"
                index += 1
            else:
                index += 1
        else:
            if character in "\r\n":
                previous = source[index - 1] if index else ""
                if previous == "\\":
                    index += 2 if source.startswith("\r\n", index) else 1
                else:
                    state = "normal"
                    index += 1
            else:
                index += 1
    return ranges


def format_source(source: str) -> tuple[str, list[tuple[int, str]]]:
    newline_match = re.search(r"\r\n|\n|\r", source)
    newline = newline_match.group(0) if newline_match else os.linesep
    ranges = _find_doc_comments(source)
    warnings: list[tuple[int, str]] = []
    pieces: list[str] = []
    cursor = 0
    line_number = 1

    for start, end in ranges:
        unchanged = source[cursor:start]
        pieces.append(unchanged)
        line_number += len(re.findall(r"\r\n|\n|\r", unchanged))
        formatted, comment_warnings = _format_doc_comment(
            source[start:end], line_number, newline
        )
        pieces.append(formatted)
        warnings.extend(comment_warnings)
        line_number += len(re.findall(r"\r\n|\n|\r", source[start:end]))
        cursor = end
    pieces.append(source[cursor:])
    return "".join(pieces), warnings


def _atomic_write(path: Path, content: bytes, mode: int) -> None:
    temporary_name: str | None = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as temporary:
            temporary_name = temporary.name
            temporary.write(content)
            temporary.flush()
            os.fsync(temporary.fileno())
        os.chmod(temporary_name, stat.S_IMODE(mode))
        os.replace(temporary_name, path)
    finally:
        if temporary_name is not None:
            try:
                os.unlink(temporary_name)
            except FileNotFoundError:
                pass


def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Format @brief, @param and @return paragraphs in one C file."
    )
    parser.add_argument("file", type=Path, help="UTF-8 .c file to modify in place")
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = _argument_parser().parse_args(argv)
    path = arguments.file
    if path.suffix != ".c":
        print(f"error: expected a .c file: {path}", file=sys.stderr)
        return 2
    if path.is_symlink() or not path.is_file():
        print(f"error: not a regular file: {path}", file=sys.stderr)
        return 2

    try:
        original_bytes = path.read_bytes()
        source = original_bytes.decode("utf-8")
        formatted, warnings = format_source(source)
        formatted_bytes = formatted.encode("utf-8")
        if formatted_bytes != original_bytes:
            _atomic_write(path, formatted_bytes, path.stat().st_mode)
    except (FormatError, OSError, UnicodeError) as error:
        print(f"error: {path}: {error}", file=sys.stderr)
        return 2

    for line_number, token in warnings:
        print(
            f"warning: {path}:{line_number}: content cannot be safely wrapped "
            f"to {COLUMN_LIMIT} columns: {token}",
            file=sys.stderr,
        )
    return 1 if warnings else 0


if __name__ == "__main__":
    raise SystemExit(main())

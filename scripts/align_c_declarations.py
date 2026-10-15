"""Align initializers across uninitialized, single-line C declarations.

This deliberately recognizes only simple, single-declarator statements. Anything
ambiguous ends the group rather than trying to implement a complete C parser.
Only whitespace immediately before an initializer's '=' can change.
"""

from __future__ import annotations

from dataclasses import dataclass
import re
import unicodedata


_IDENTIFIER = re.compile(r"[A-Za-z_]\w*", re.ASCII)
_FORMAT_DIRECTIVE = re.compile(r"(?:/\*|//)\s*clang-format\s+(off|on)\b")
_QUALIFIERS = {"const", "volatile", "restrict", "_Atomic"}
_STORAGE = {"auto", "extern", "register", "static", "_Thread_local"}
_BUILTINS = {
    "void", "char", "short", "int", "long", "float", "double", "signed",
    "unsigned", "_Bool", "bool", "_Complex", "_Imaginary", "__int128",
}
_KEYWORDS = _QUALIFIERS | _STORAGE | _BUILTINS | {
    "break", "case", "continue", "default", "do", "else", "enum", "for",
    "goto", "if", "inline", "return", "sizeof", "struct", "switch",
    "typedef", "union", "while", "_Alignas", "_Alignof", "_Generic",
    "_Noreturn", "_Static_assert", "asm", "__asm__", "__attribute__",
    "__declspec", "true", "false", "NULL",
}
_OPERATORS = re.compile(
    r"<<=|>>=|\.\.\.|->|\+\+|--|&&|\|\||<=|>=|==|!=|"
    r"\+=|-=|\*=|/=|%=|&=|\|=|\^=|<<|>>|##|."
)


@dataclass(frozen=True)
class _Token:
    text: str
    start: int
    kind: str = "symbol"


@dataclass(frozen=True)
class _Line:
    text: str
    ending: str
    tokens: list[_Token]
    safe: bool


@dataclass(frozen=True)
class _Declaration:
    equals: int | None
    indent: str


def _line_parts(raw: str) -> tuple[str, str]:
    text = raw.rstrip("\r\n")
    return text, raw[len(text):]


def _logical_lines(source: str):
    """Splice C's escaped newlines before recognizing comments or strings.

    A spliced logical line is never edited, but must still update lexer state:
    even a comment delimiter can straddle an escaped newline.
    """
    physical: list[str] = []
    logical = ""
    for raw in source.splitlines(keepends=True):
        physical.append(raw)
        text, ending = _line_parts(raw)
        if ending and text.endswith("\\"):
            logical += text[:-1]
            continue
        yield physical, logical + text
        physical = []
        logical = ""
    if physical:
        yield physical, logical


def _lex_lines(source: str) -> list[_Line]:
    """Keep token positions while protecting strings, comments and directives."""
    lines: list[_Line] = []
    block_comment = False
    quote: str | None = None
    disabled = False

    for physical, text in _logical_lines(source):
        tokens: list[_Token] = []
        safe = not (disabled or block_comment or quote or len(physical) > 1)
        if text.lstrip().startswith("#"):
            safe = False
        if text.rstrip().endswith("\\"):
            safe = False
        index = 0
        while index < len(text):
            if block_comment:
                close = text.find("*/", index)
                if close < 0:
                    break
                block_comment = False
                index = close + 2
                continue

            if quote is not None:
                safe = False
                while index < len(text):
                    if text[index] == "\\":
                        index += 2
                    elif text[index] == quote:
                        quote = None
                        index += 1
                        break
                    else:
                        index += 1
                continue

            if text[index].isspace():
                index += 1
                continue
            if text.startswith(("//", "/*"), index):
                directive = _FORMAT_DIRECTIVE.match(text, index)
                if directive:
                    disabled = directive.group(1) == "off"
                    safe = False
                if text.startswith("//", index):
                    break
                close = text.find("*/", index + 2)
                if close < 0:
                    block_comment = True
                    safe = False
                    break
                index = close + 2
                continue

            if text[index] in {'"', "'"}:
                start = index
                delimiter = text[index]
                index += 1
                while index < len(text):
                    if text[index] == "\\":
                        index += 2
                    elif text[index] == delimiter:
                        index += 1
                        break
                    else:
                        index += 1
                else:
                    quote = delimiter
                    safe = False
                tokens.append(_Token(text[start:index], start, "literal"))
                continue

            identifier = _IDENTIFIER.match(text, index)
            if identifier:
                tokens.append(_Token(identifier.group(), index, "identifier"))
                index = identifier.end()
                continue
            operator = _OPERATORS.match(text, index)
            assert operator is not None
            tokens.append(_Token(operator.group(), index))
            index = operator.end()

        for raw in physical:
            original, ending = _line_parts(raw)
            lines.append(_Line(original, ending, tokens if len(physical) == 1 else [], safe))
    return lines


def _balanced_end(tokens: list[_Token], start: int) -> int | None:
    pairs = {"(": ")", "[": "]", "{": "}"}
    stack: list[str] = []
    for index in range(start, len(tokens)):
        text = tokens[index].text
        if tokens[index].kind == "literal":
            continue
        if text in pairs:
            stack.append(pairs[text])
        elif text in pairs.values():
            if not stack or stack.pop() != text:
                return None
            if not stack:
                return index + 1
    return None


def _declaration(line: _Line) -> _Declaration | None:
    tokens = line.tokens
    if not line.safe or not tokens or tokens[-1].text != ";":
        return None

    index = 0
    qualified = False
    while index < len(tokens) and tokens[index].text in _STORAGE | _QUALIFIERS:
        qualified = True
        index += 1
    if index == len(tokens):
        return None

    certain_type = qualified
    text = tokens[index].text
    if text in {"struct", "union", "enum"}:
        index += 1
        if index >= len(tokens) or tokens[index].kind != "identifier":
            return None
        index += 1
        certain_type = True
    elif text in _BUILTINS:
        while index < len(tokens) and tokens[index].text in _BUILTINS | _QUALIFIERS:
            index += 1
        certain_type = True
    elif tokens[index].kind == "identifier" and text not in _KEYWORDS:
        index += 1
    else:
        return None

    while index < len(tokens) and tokens[index].text in _QUALIFIERS:
        index += 1
    pointer = False
    while index < len(tokens) and tokens[index].text == "*":
        pointer = True
        index += 1
        while index < len(tokens) and tokens[index].text in _QUALIFIERS:
            index += 1
    if (index >= len(tokens) or tokens[index].kind != "identifier"
            or tokens[index].text in _KEYWORDS):
        return None
    index += 1

    while index < len(tokens) and tokens[index].text == "[":
        end = _balanced_end(tokens, index)
        if end is None:
            return None
        index = end

    equals: int | None = None
    if index < len(tokens) and tokens[index].text == "=":
        equals = tokens[index].start
        index += 1
        first = index
        while index < len(tokens) - 1:
            text = tokens[index].text
            if tokens[index].kind != "literal":
                if text in {"(", "[", "{"}:
                    end = _balanced_end(tokens, index)
                    if end is None:
                        return None
                    index = end
                    continue
                if text in {",", ";", ")", "]", "}"}:
                    return None
            index += 1
        if index == first:
            return None

    if index != len(tokens) - 1:
        return None
    # An unknown `a * b;` could be multiplication rather than a declaration.
    # Even a known typedef may be shadowed by a variable in an inner scope, so
    # only explicit specifiers or an initializer remove that ambiguity here.
    if pointer and equals is None and not certain_type:
        return None
    indent = line.text[:len(line.text) - len(line.text.lstrip(" \t"))]
    return _Declaration(equals, indent)


def _display_width(text: str, tab_width: int) -> int:
    column = 0
    for character in text:
        if character == "\t":
            column += tab_width - column % tab_width
        elif unicodedata.category(character) in {"Mn", "Me"}:
            continue
        elif unicodedata.east_asian_width(character) in {"W", "F"}:
            column += 2
        else:
            column += 1
    return column


def align_declaration_initializers(
    source: str,
    *,
    line_ranges: list[tuple[int, int]] | None = None,
    column_limit: int = 100,
    tab_width: int = 8,
) -> str:
    """Align '=' in complete declaration groups inside the requested line ranges.

    Ranges are one-based and inclusive. Groups touching unselected declarations
    are left alone. A zero column limit means unlimited, as in clang-format.
    """
    if tab_width <= 0 or column_limit < 0:
        raise ValueError("invalid column limit or tab width")
    lines = _lex_lines(source)
    declarations = [_declaration(line) for line in lines]
    output = [line.text + line.ending for line in lines]
    ranges = sorted(line_ranges) if line_ranges is not None else None

    def selected(first: int, last: int) -> bool:
        if ranges is None:
            return True
        cursor = first
        for start, end in ranges:
            if start > cursor:
                return False
            if end >= cursor:
                cursor = end + 1
            if cursor > last:
                return True
        return False

    first = 0
    while first < len(lines):
        declaration = declarations[first]
        if declaration is None:
            first += 1
            continue
        last = first + 1
        while (last < len(lines) and declarations[last] is not None
               and declarations[last].indent == declaration.indent):
            last += 1
        initialized = [index for index in range(first, last)
                       if declarations[index].equals is not None]
        if len(initialized) >= 2 and selected(first + 1, last):
            prefixes = {
                index: lines[index].text[:declarations[index].equals].rstrip(" \t")
                for index in initialized
            }
            column = max(_display_width(prefix, tab_width) for prefix in prefixes.values()) + 1
            replacements = {
                index: prefix + " " * (column - _display_width(prefix, tab_width))
                + lines[index].text[declarations[index].equals:]
                for index, prefix in prefixes.items()
            }
            if not column_limit or all(
                _display_width(text, tab_width) <= column_limit for text in replacements.values()
            ):
                for index, text in replacements.items():
                    output[index] = text + lines[index].ending
        first = last
    return "".join(output)

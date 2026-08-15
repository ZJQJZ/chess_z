'use strict';

const DEFAULT_WRAPPER_PAIRS = Object.freeze([
  ['(', ')'],
  ['[', ']'],
  ['{', '}'],
  ['<', '>'],
  ['（', '）'],
  ['［', '］'],
  ['｛', '｝'],
  ['＜', '＞'],
  ['【', '】'],
  ['〔', '〕'],
  ['〖', '〗'],
  ['〘', '〙'],
  ['〚', '〛'],
  ['〈', '〉'],
  ['《', '》'],
  ['"', '"'],
  ["'", "'"],
  ['“', '”'],
  ['‘', '’'],
  ['„', '“'],
  ['‚', '‘'],
  ['«', '»'],
  ['‹', '›'],
  ['「', '」'],
  ['『', '』'],
  ['〝', '〞'],
  ['＂', '＂'],
  ['＇', '＇'],
  ['`', '`'],
  ['```', '```'],
  ["'''", "'''"],
  ['"""', '"""'],
]);

const MARKDOWN_WRAPPER_PAIRS = Object.freeze([
  ['**', '**'],
  ['__', '__'],
  ['~~', '~~'],
]);

const DEFAULT_OPTIONS = Object.freeze({
  includeLine: true,
  wrapperPairs: DEFAULT_WRAPPER_PAIRS,
  enableMarkdownPairs: false,
  sentenceTerminators: '.!?。！？；;…',
  maxParseCharacters: 1_000_000,
});

const WORD_CHARACTER = /[\p{L}\p{M}\p{N}_]/u;
const WHITESPACE = /\s/u;
const ASYMMETRIC_QUOTE_PAIRS = new Set([
  '“\u0000”',
  '‘\u0000’',
  '„\u0000“',
  '‚\u0000‘',
  '«\u0000»',
  '‹\u0000›',
  '「\u0000」',
  '『\u0000』',
  '〝\u0000〞',
]);

let wordSegmenter;

function getWordSegmenter() {
  if (wordSegmenter !== undefined) {
    return wordSegmenter;
  }

  wordSegmenter = typeof Intl.Segmenter === 'function'
    ? new Intl.Segmenter(undefined, { granularity: 'word' })
    : null;
  return wordSegmenter;
}

function normalizeRange(range, textLength) {
  const rawStart = Number.isFinite(range?.start) ? Math.trunc(range.start) : 0;
  const rawEnd = Number.isFinite(range?.end) ? Math.trunc(range.end) : rawStart;
  const start = Math.max(0, Math.min(textLength, Math.min(rawStart, rawEnd)));
  const end = Math.max(0, Math.min(textLength, Math.max(rawStart, rawEnd)));
  return { start, end };
}

function normalizeOptions(options = {}) {
  const configuredPairs = Array.isArray(options.wrapperPairs)
    ? options.wrapperPairs
    : DEFAULT_WRAPPER_PAIRS;
  const pairs = configuredPairs
    .filter((pair) => Array.isArray(pair)
      && pair.length === 2
      && typeof pair[0] === 'string'
      && typeof pair[1] === 'string'
      && pair[0].length > 0
      && pair[1].length > 0)
    .map(([open, close]) => [open, close]);

  if (options.enableMarkdownPairs === true) {
    pairs.push(...MARKDOWN_WRAPPER_PAIRS.map(([open, close]) => [open, close]));
  }

  const deduplicatedPairs = [];
  const seenPairs = new Set();
  for (const [open, close] of pairs) {
    const key = `${open}\u0000${close}`;
    if (!seenPairs.has(key)) {
      seenPairs.add(key);
      deduplicatedPairs.push([open, close]);
    }
  }

  return {
    includeLine: options.includeLine !== false,
    wrapperPairs: deduplicatedPairs,
    sentenceTerminators: typeof options.sentenceTerminators === 'string'
      ? options.sentenceTerminators
      : DEFAULT_OPTIONS.sentenceTerminators,
    maxParseCharacters: Number.isFinite(options.maxParseCharacters)
      ? Math.max(1_000, Math.trunc(options.maxParseCharacters))
      : DEFAULT_OPTIONS.maxParseCharacters,
  };
}

function splitLines(text) {
  const lines = [];
  let start = 0;
  let index = 0;

  while (index < text.length) {
    if (text[index] !== '\n' && text[index] !== '\r') {
      index += 1;
      continue;
    }

    const end = index;
    if (text[index] === '\r' && text[index + 1] === '\n') {
      index += 2;
    } else {
      index += 1;
    }
    lines.push({
      start,
      end,
      endIncludingEol: index,
      blank: /^\s*$/u.test(text.slice(start, end)),
    });
    start = index;
  }

  lines.push({
    start,
    end: text.length,
    endIncludingEol: text.length,
    blank: /^\s*$/u.test(text.slice(start)),
  });
  return lines;
}

function lineIndexAtOffset(lines, offset) {
  let low = 0;
  let high = lines.length - 1;

  while (low <= high) {
    const middle = Math.floor((low + high) / 2);
    const line = lines[middle];
    const nextStart = middle + 1 < lines.length
      ? lines[middle + 1].start
      : Number.POSITIVE_INFINITY;

    if (offset < line.start) {
      high = middle - 1;
    } else if (offset >= nextStart) {
      low = middle + 1;
    } else {
      return middle;
    }
  }

  return Math.max(0, Math.min(lines.length - 1, low));
}

function findParagraphContext(text, range) {
  const lines = splitLines(text);
  const startLineIndex = lineIndexAtOffset(lines, range.start);
  const endProbe = range.end > range.start ? range.end - 1 : range.end;
  const endLineIndex = lineIndexAtOffset(lines, endProbe);

  if (startLineIndex !== endLineIndex) {
    for (let index = startLineIndex; index <= endLineIndex; index += 1) {
      if (lines[index].blank) {
        return null;
      }
    }
  }

  if (lines[startLineIndex].blank || lines[endLineIndex].blank) {
    if (startLineIndex !== endLineIndex) {
      return null;
    }
    return {
      lines,
      startLineIndex,
      endLineIndex,
      paragraph: {
        start: lines[startLineIndex].start,
        end: lines[startLineIndex].end,
      },
    };
  }

  let firstLineIndex = startLineIndex;
  while (firstLineIndex > 0 && !lines[firstLineIndex - 1].blank) {
    firstLineIndex -= 1;
  }

  let lastLineIndex = endLineIndex;
  while (lastLineIndex + 1 < lines.length && !lines[lastLineIndex + 1].blank) {
    lastLineIndex += 1;
  }

  return {
    lines,
    startLineIndex,
    endLineIndex,
    paragraph: {
      start: lines[firstLineIndex].start,
      end: lines[lastLineIndex].end,
    },
  };
}

function containsRange(container, contained) {
  return container.start <= contained.start && container.end >= contained.end;
}

function isStrictSuperset(container, contained) {
  return containsRange(container, contained)
    && (container.start < contained.start || container.end > contained.end);
}

function isWordCharacter(character) {
  return typeof character === 'string' && character.length > 0 && WORD_CHARACTER.test(character);
}

function addWordCandidates(candidates, text, line, currentRange) {
  if (currentRange.start < line.start || currentRange.end > line.end) {
    return;
  }

  const lineText = text.slice(line.start, line.end);
  const segmenter = getWordSegmenter();
  const rawSegments = [];

  if (segmenter) {
    for (const segment of segmenter.segment(lineText)) {
      if (segment.isWordLike || /^_+$/u.test(segment.segment)) {
        rawSegments.push({
          start: segment.index,
          end: segment.index + segment.segment.length,
          underscore: /^_+$/u.test(segment.segment),
        });
      }
    }
  } else {
    let start = null;
    for (let index = 0; index < lineText.length;) {
      const character = String.fromCodePoint(lineText.codePointAt(index));
      if (isWordCharacter(character)) {
        start ??= index;
      } else if (start !== null) {
        rawSegments.push({ start, end: index, underscore: false });
        start = null;
      }
      index += character.length;
    }
    if (start !== null) {
      rawSegments.push({ start, end: lineText.length, underscore: false });
    }
  }

  const mergedSegments = [];
  for (let index = 0; index < rawSegments.length;) {
    const first = rawSegments[index];
    let end = first.end;
    let hasWord = !first.underscore;
    let cursor = index + 1;

    while (cursor < rawSegments.length && rawSegments[cursor].start === end) {
      end = rawSegments[cursor].end;
      hasWord ||= !rawSegments[cursor].underscore;
      cursor += 1;
    }

    if (hasWord) {
      mergedSegments.push({ start: first.start, end });
    }
    index = cursor;
  }

  for (const segment of mergedSegments) {
    const candidate = {
      start: line.start + segment.start,
      end: line.start + segment.end,
      kind: 'word',
      priority: 10,
    };
    if (containsRange(candidate, currentRange)) {
      candidates.push(candidate);
    }
  }
}

function isEscaped(text, index) {
  let backslashes = 0;
  for (let cursor = index - 1; cursor >= 0 && text[cursor] === '\\'; cursor -= 1) {
    backslashes += 1;
  }
  return backslashes % 2 === 1;
}

function isWordInternalApostrophe(text, index, token) {
  if (token !== "'") {
    return false;
  }

  const previous = previousCodePoint(text, index);
  const nextOffset = index + token.length;
  const next = nextOffset < text.length
    ? String.fromCodePoint(text.codePointAt(nextOffset))
    : '';
  return isWordCharacter(previous) && isWordCharacter(next);
}

function matchingTokenAt(text, index, tokens) {
  for (const token of tokens) {
    if (text.startsWith(token, index)) {
      return token;
    }
  }
  return null;
}

function parseWrapperRanges(text, pairs) {
  const definitions = pairs.map(([open, close], id) => ({
    id,
    open,
    close,
    quote: open === close || ASYMMETRIC_QUOTE_PAIRS.has(`${open}\u0000${close}`),
  }));
  const openTokens = [...new Set(definitions.map((pair) => pair.open))]
    .sort((left, right) => right.length - left.length);
  const closeTokens = [...new Set(definitions.map((pair) => pair.close))]
    .sort((left, right) => right.length - left.length);
  const definitionsByOpen = new Map();

  for (const definition of definitions) {
    const existing = definitionsByOpen.get(definition.open) ?? [];
    existing.push(definition);
    definitionsByOpen.set(definition.open, existing);
  }

  const stack = [];
  const ranges = [];
  let index = 0;

  while (index < text.length) {
    const top = stack[stack.length - 1];
    if (top
      && text.startsWith(top.definition.close, index)
      && !isEscaped(text, index)
      && !isWordInternalApostrophe(text, index, top.definition.close)) {
      const closeStart = index;
      const closeEnd = index + top.definition.close.length;
      stack.pop();

      const angleBracketIsPlausible = top.definition.open !== '<'
        || (closeStart > top.openEnd
          && !WHITESPACE.test(text[top.openEnd])
          && !WHITESPACE.test(text[closeStart - 1]));

      if (angleBracketIsPlausible) {
        ranges.push({
          inner: { start: top.openEnd, end: closeStart },
          outer: { start: top.openStart, end: closeEnd },
        });
      }
      index = closeEnd;
      continue;
    }

    let containingQuoteIndex = -1;
    for (let stackIndex = stack.length - 1; stackIndex >= 0; stackIndex -= 1) {
      if (stack[stackIndex].definition.quote) {
        containingQuoteIndex = stackIndex;
        break;
      }
    }

    const containingQuote = stack[containingQuoteIndex];
    if (containingQuote
      && stack.slice(containingQuoteIndex + 1).every((entry) => !entry.definition.quote)
      && text.startsWith(containingQuote.definition.close, index)
      && !isEscaped(text, index)
      && !isWordInternalApostrophe(text, index, containingQuote.definition.close)) {
      const closeStart = index;
      const closeEnd = index + containingQuote.definition.close.length;
      stack.splice(containingQuoteIndex);
      ranges.push({
        inner: { start: containingQuote.openEnd, end: closeStart },
        outer: { start: containingQuote.openStart, end: closeEnd },
      });
      index = closeEnd;
      continue;
    }

    const openToken = matchingTokenAt(text, index, openTokens);
    if (openToken
      && !isEscaped(text, index)
      && !isWordInternalApostrophe(text, index, openToken)) {
      const possibleDefinitions = definitionsByOpen.get(openToken);
      const definition = possibleDefinitions[0];
      stack.push({
        definition,
        openStart: index,
        openEnd: index + openToken.length,
      });
      index += openToken.length;
      continue;
    }

    const closeToken = matchingTokenAt(text, index, closeTokens);
    if (closeToken && !isEscaped(text, index)) {
      let crossingIndex = -1;
      for (let stackIndex = stack.length - 1; stackIndex >= 0; stackIndex -= 1) {
        if (stack[stackIndex].definition.close === closeToken) {
          crossingIndex = stackIndex;
          break;
        }
      }
      if (crossingIndex >= 0) {
        stack.splice(crossingIndex);
      }
      index += closeToken.length;
      continue;
    }

    const character = String.fromCodePoint(text.codePointAt(index));
    index += character.length;
  }

  return ranges;
}

function previousCodePoint(text, index) {
  if (index <= 0) {
    return '';
  }
  const previousUnit = text.charCodeAt(index - 1);
  const start = previousUnit >= 0xDC00 && previousUnit <= 0xDFFF ? index - 2 : index - 1;
  return String.fromCodePoint(text.codePointAt(Math.max(0, start)));
}

function nextCodePoint(text, index) {
  if (index >= text.length) {
    return '';
  }
  return String.fromCodePoint(text.codePointAt(index));
}

function isSentenceTerminator(text, index, terminators) {
  const character = nextCodePoint(text, index);
  if (!terminators.has(character)) {
    return false;
  }

  if (character === '.') {
    const previous = previousCodePoint(text, index);
    const next = nextCodePoint(text, index + character.length);
    if (isWordCharacter(previous) && isWordCharacter(next)) {
      return false;
    }
  }
  return true;
}

function trimRange(text, start, end) {
  while (start < end) {
    const character = nextCodePoint(text, start);
    if (!WHITESPACE.test(character)) {
      break;
    }
    start += character.length;
  }

  while (end > start) {
    const character = previousCodePoint(text, end);
    if (!WHITESPACE.test(character)) {
      break;
    }
    end -= character.length;
  }
  return { start, end };
}

function parseSentenceRanges(text, terminatorCharacters, pairs) {
  const terminators = new Set([...terminatorCharacters]);
  if (terminators.size === 0) {
    const range = trimRange(text, 0, text.length);
    return range.start < range.end ? [range] : [];
  }

  const closingTokens = [...new Set(pairs.map(([, close]) => close))]
    .sort((left, right) => right.length - left.length);
  const ranges = [];
  let sentenceStart = 0;
  let index = 0;

  while (index < text.length) {
    const character = nextCodePoint(text, index);
    if (!isSentenceTerminator(text, index, terminators)) {
      index += character.length;
      continue;
    }

    let sentenceEnd = index + character.length;
    while (sentenceEnd < text.length) {
      const next = nextCodePoint(text, sentenceEnd);
      if (!isSentenceTerminator(text, sentenceEnd, terminators)) {
        break;
      }
      sentenceEnd += next.length;
    }

    while (sentenceEnd < text.length) {
      const closer = matchingTokenAt(text, sentenceEnd, closingTokens);
      if (!closer) {
        break;
      }
      sentenceEnd += closer.length;
    }

    const range = trimRange(text, sentenceStart, sentenceEnd);
    if (range.start < range.end) {
      ranges.push(range);
    }
    sentenceStart = sentenceEnd;
    index = sentenceEnd;
  }

  const remainder = trimRange(text, sentenceStart, text.length);
  if (remainder.start < remainder.end) {
    ranges.push(remainder);
  }
  return ranges;
}

function deduplicateAndSortCandidates(candidates, currentRange) {
  const unique = new Map();
  for (const candidate of candidates) {
    if (!isStrictSuperset(candidate, currentRange)) {
      continue;
    }
    const key = `${candidate.start}:${candidate.end}`;
    const previous = unique.get(key);
    if (!previous || candidate.priority < previous.priority) {
      unique.set(key, candidate);
    }
  }

  return [...unique.values()].sort((left, right) => {
    const lengthDifference = (left.end - left.start) - (right.end - right.start);
    if (lengthDifference !== 0) {
      return lengthDifference;
    }
    if (left.priority !== right.priority) {
      return left.priority - right.priority;
    }
    return right.start - left.start;
  });
}

function expandRange(text, inputRange, inputOptions = {}) {
  const range = normalizeRange(inputRange, text.length);
  const options = normalizeOptions(inputOptions);
  const context = findParagraphContext(text, range);
  if (!context) {
    return range;
  }

  const candidates = [];
  if (context.startLineIndex === context.endLineIndex) {
    const line = context.lines[context.startLineIndex];
    addWordCandidates(candidates, text, line, range);
    if (options.includeLine) {
      candidates.push({ start: line.start, end: line.end, kind: 'line', priority: 50 });
    }
  }

  const paragraph = context.paragraph;
  const paragraphText = text.slice(paragraph.start, paragraph.end);
  if (paragraphText.length <= options.maxParseCharacters) {
    for (const wrapper of parseWrapperRanges(paragraphText, options.wrapperPairs)) {
      candidates.push({
        start: paragraph.start + wrapper.inner.start,
        end: paragraph.start + wrapper.inner.end,
        kind: 'wrapper-inner',
        priority: 20,
      });
      candidates.push({
        start: paragraph.start + wrapper.outer.start,
        end: paragraph.start + wrapper.outer.end,
        kind: 'wrapper-outer',
        priority: 30,
      });
    }

    for (const sentence of parseSentenceRanges(
      paragraphText,
      options.sentenceTerminators,
      options.wrapperPairs,
    )) {
      candidates.push({
        start: paragraph.start + sentence.start,
        end: paragraph.start + sentence.end,
        kind: 'sentence',
        priority: 40,
      });
    }
  }

  candidates.push({ ...paragraph, kind: 'paragraph', priority: 60 });
  const sorted = deduplicateAndSortCandidates(candidates, range);
  if (sorted.length === 0) {
    return range;
  }
  return { start: sorted[0].start, end: sorted[0].end };
}

function expansionChain(text, inputRange, inputOptions = {}) {
  const chain = [normalizeRange(inputRange, text.length)];
  while (true) {
    const next = expandRange(text, chain[chain.length - 1], inputOptions);
    const previous = chain[chain.length - 1];
    if (next.start === previous.start && next.end === previous.end) {
      return chain;
    }
    chain.push(next);
  }
}

module.exports = {
  DEFAULT_OPTIONS,
  DEFAULT_WRAPPER_PAIRS,
  MARKDOWN_WRAPPER_PAIRS,
  expandRange,
  expansionChain,
  normalizeOptions,
  parseSentenceRanges,
  parseWrapperRanges,
};

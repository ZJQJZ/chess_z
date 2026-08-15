'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const {
  DEFAULT_WRAPPER_PAIRS,
  expandRange,
  expansionChain,
  normalizeOptions,
  parseSentenceRanges,
  parseWrapperRanges,
} = require('../selection-engine');
const {
  SelectionHistory,
  snapshotsEqual,
} = require('../selection-history');

function chainText(text, offset, options) {
  return expansionChain(text, { start: offset, end: offset }, options)
    .slice(1)
    .map((range) => text.slice(range.start, range.end));
}

function rangesText(text, ranges) {
  return ranges.map((range) => text.slice(range.start, range.end));
}

test('expands English words and partial selections', () => {
  const text = 'alpha beta.';
  assert.deepEqual(chainText(text, text.indexOf('beta') + 1), [
    'beta',
    'alpha beta.',
  ]);

  assert.deepEqual(
    expandRange(text, { start: 7, end: 9 }),
    { start: 6, end: 10 },
  );
});

test('treats underscore-connected identifiers as one Unicode word', () => {
  const text = 'before snake_case 后面';
  const chain = chainText(text, text.indexOf('case'));
  assert.equal(chain[0], 'snake_case');
  assert.ok(chain.includes(text));
});

test('expands Chinese text through quote interior, quote exterior, and sentence', () => {
  const text = '这是“测试内容”。下一句！';
  const chain = chainText(text, text.indexOf('内容'));
  assert.deepEqual(chain.slice(0, 3), [
    '测试内容',
    '“测试内容”',
    '这是“测试内容”。',
  ]);
  assert.equal(chain.at(-1), text);
});

test('adds separate inner and outer levels for nested wrappers', () => {
  const text = 'fn({"k": `value`})';
  assert.deepEqual(chainText(text, text.indexOf('value') + 1), [
    'value',
    '`value`',
    '"k": `value`',
    '{"k": `value`}',
    '({"k": `value`})',
    text,
  ]);
});

test('keeps whitespace in a wrapper interior', () => {
  const text = '(  alpha  ) suffix.';
  assert.deepEqual(chainText(text, text.indexOf('alpha') + 1).slice(0, 3), [
    'alpha',
    '  alpha  ',
    '(  alpha  )',
  ]);
});

test('handles escaped quotes and ignores apostrophes inside words', () => {
  const quoted = 'He said "a \\"quoted\\" word". Next.';
  const wrappers = parseWrapperRanges(quoted, DEFAULT_WRAPPER_PAIRS);
  const outerTexts = wrappers.map(({ outer }) => quoted.slice(outer.start, outer.end));
  assert.ok(outerTexts.includes('"a \\"quoted\\" word"'));

  const apostropheText = "don't stop.";
  assert.deepEqual(chainText(apostropheText, 2), ["don't", apostropheText]);
  assert.equal(parseWrapperRanges(apostropheText, DEFAULT_WRAPPER_PAIRS).length, 0);
});

test('recognizes triple quotes and triple backticks before single delimiters', () => {
  for (const text of ['```code```', "'''code'''", '"""code"""']) {
    const wrappers = parseWrapperRanges(text, DEFAULT_WRAPPER_PAIRS);
    assert.equal(wrappers.length, 1);
    assert.equal(text.slice(wrappers[0].inner.start, wrappers[0].inner.end), 'code');
    assert.equal(text.slice(wrappers[0].outer.start, wrappers[0].outer.end), text);
  }
});

test('contains unbalanced brackets within quotes and supports nested smart quotes', () => {
  const unmatchedBracket = '"value ) still"';
  const firstWrappers = parseWrapperRanges(unmatchedBracket, DEFAULT_WRAPPER_PAIRS);
  assert.ok(firstWrappers.some(({ outer }) => (
    unmatchedBracket.slice(outer.start, outer.end) === unmatchedBracket
  )));

  const balancedBracket = '"value (inside) still"';
  const balancedWrappers = parseWrapperRanges(balancedBracket, DEFAULT_WRAPPER_PAIRS);
  const balancedTexts = balancedWrappers.map(({ outer }) => (
    balancedBracket.slice(outer.start, outer.end)
  ));
  assert.ok(balancedTexts.includes('(inside)'));
  assert.ok(balancedTexts.includes(balancedBracket));

  const nestedQuotes = '“outer ‘inner’ end”';
  const secondWrappers = parseWrapperRanges(nestedQuotes, DEFAULT_WRAPPER_PAIRS);
  const outerTexts = secondWrappers.map(({ outer }) => (
    nestedQuotes.slice(outer.start, outer.end)
  ));
  assert.deepEqual(outerTexts.sort(), ['‘inner’', nestedQuotes].sort());
});

test('rejects crossed or unclosed wrapper pairs', () => {
  assert.equal(parseWrapperRanges('([value)]', DEFAULT_WRAPPER_PAIRS).length, 0);
  assert.equal(parseWrapperRanges('(value', DEFAULT_WRAPPER_PAIRS).length, 0);
});

test('recognizes plausible ASCII angles but not spaced comparisons', () => {
  assert.deepEqual(chainText('<value> tail', 2).slice(0, 2), ['value', '<value>']);

  const comparison = 'a < b && c > d';
  const wrappers = parseWrapperRanges(comparison, DEFAULT_WRAPPER_PAIRS);
  assert.equal(wrappers.some(({ outer }) => (
    comparison.slice(outer.start, outer.end) === '< b && c >'
  )), false);
});

test('splits multilingual sentences, preserves punctuation runs and closing quotes', () => {
  const text = 'Value 1.2 works... Really?! “可以！” Next';
  const ranges = parseSentenceRanges(text, '.!?。！？；;…', DEFAULT_WRAPPER_PAIRS);
  assert.deepEqual(rangesText(text, ranges), [
    'Value 1.2 works...',
    'Really?!',
    '“可以！”',
    'Next',
  ]);
});

test('uses the physical line before the containing multi-line paragraph', () => {
  const text = 'first line\nsecond target line\n\nnext paragraph';
  assert.deepEqual(chainText(text, text.indexOf('target') + 1), [
    'target',
    'second target line',
    'first line\nsecond target line',
  ]);
});

test('can disable the physical-line level', () => {
  const text = 'first line\nsecond target line';
  assert.deepEqual(chainText(text, text.indexOf('target'), { includeLine: false }), [
    'target',
    text,
  ]);
});

test('stops at a paragraph and does not expand a cross-paragraph selection', () => {
  const text = 'first paragraph\n\nsecond paragraph';
  const firstParagraph = { start: 0, end: 'first paragraph'.length };
  assert.deepEqual(expandRange(text, firstParagraph), firstParagraph);

  const crossParagraph = { start: 3, end: text.indexOf('second') + 3 };
  assert.deepEqual(expandRange(text, crossParagraph), crossParagraph);
});

test('skips sentence and wrapper parsing over the configured size limit', () => {
  const inner = 'x'.repeat(1_100);
  const text = `(${inner}) suffix`;
  const chain = chainText(text, 10, { maxParseCharacters: 1_000 });
  assert.deepEqual(chain, [inner, text]);
});

test('supports configurable pairs and optional Markdown wrappers', () => {
  const custom = normalizeOptions({ wrapperPairs: [['BEGIN', 'END']] });
  const customText = 'BEGINvalueEND rest';
  const customWrappers = parseWrapperRanges(customText, custom.wrapperPairs);
  assert.equal(customText.slice(
    customWrappers[0].outer.start,
    customWrappers[0].outer.end,
  ), 'BEGINvalueEND');

  assert.deepEqual(chainText('**bold** rest', 3, {
    enableMarkdownPairs: true,
  }).slice(0, 2), ['bold', '**bold**']);
});

test('keeps manifest defaults synchronized with engine defaults', () => {
  const manifest = JSON.parse(fs.readFileSync(
    path.join(__dirname, '..', 'package.json'),
    'utf8',
  ));
  const properties = manifest.contributes.configuration.properties;
  assert.deepEqual(properties['smartExpand.wrapperPairs'].default, DEFAULT_WRAPPER_PAIRS);
  assert.equal(
    properties['smartExpand.sentenceTerminators'].default,
    normalizeOptions().sentenceTerminators,
  );
  assert.equal(
    properties['smartExpand.maxParseCharacters'].default,
    normalizeOptions().maxParseCharacters,
  );
});

test('expands independent ranges for multi-cursor use without changing direction data', () => {
  const text = 'one two. three four.';
  const selections = [
    { anchor: 1, active: 1 },
    { anchor: 18, active: 16 },
  ];
  const expanded = selections.map(({ anchor, active }) => {
    const range = expandRange(text, {
      start: Math.min(anchor, active),
      end: Math.max(anchor, active),
    });
    return {
      anchor: anchor > active ? range.end : range.start,
      active: anchor > active ? range.start : range.end,
    };
  });

  assert.deepEqual(expanded, [
    { anchor: 0, active: 3 },
    { anchor: 19, active: 15 },
  ]);
});

test('selection history shrinks in order and invalidates on movement or edits', () => {
  const history = new SelectionHistory();
  const caret = [{ anchor: 2, active: 2 }];
  const word = [{ anchor: 0, active: 4 }];
  const sentence = [{ anchor: 0, active: 10 }];

  history.record('file:///test', 1, caret, word);
  history.record('file:///test', 1, word, sentence);
  assert.equal(history.length, 2);
  assert.deepEqual(history.pop('file:///test', 1, sentence).before, word);
  assert.deepEqual(history.pop('file:///test', 1, word).before, caret);
  assert.equal(history.pop('file:///test', 1, caret), null);

  history.record('file:///test', 1, caret, word);
  assert.equal(history.pop('file:///test', 1, [{ anchor: 1, active: 1 }]), null);
  assert.equal(history.length, 0);

  history.record('file:///test', 1, caret, word);
  assert.equal(history.pop('file:///test', 2, word), null);
  assert.equal(history.length, 0);
  assert.equal(snapshotsEqual(word, [{ anchor: 0, active: 4 }]), true);
});

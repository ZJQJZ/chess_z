'use strict';

const vscode = require('vscode');
const { expandRange } = require('./selection-engine');
const {
  SelectionHistory,
  snapshotSignature,
  snapshotsEqual,
} = require('./selection-history');

const historyByEditor = new WeakMap();
const expectedSelectionChanges = new WeakMap();
const trackedEditors = new Set();

function selectionSnapshot(document, selections) {
  return selections.map((selection) => ({
    anchor: document.offsetAt(selection.anchor),
    active: document.offsetAt(selection.active),
  }));
}

function historyFor(editor) {
  trackedEditors.add(editor);
  let history = historyByEditor.get(editor);
  if (!history) {
    history = new SelectionHistory();
    historyByEditor.set(editor, history);
  }
  return history;
}

function clearHistory(editor) {
  historyByEditor.delete(editor);
  expectedSelectionChanges.delete(editor);
  trackedEditors.delete(editor);
}

function clearAllHistory() {
  for (const editor of trackedEditors) {
    historyByEditor.delete(editor);
    expectedSelectionChanges.delete(editor);
  }
  trackedEditors.clear();
}

function optionsFromConfiguration() {
  const configuration = vscode.workspace.getConfiguration('smartExpand');
  return {
    includeLine: configuration.get('includeLine', true),
    wrapperPairs: configuration.get('wrapperPairs'),
    enableMarkdownPairs: configuration.get('enableMarkdownPairs', false),
    sentenceTerminators: configuration.get('sentenceTerminators', '.!?。！？；;…'),
    maxParseCharacters: configuration.get('maxParseCharacters', 1_000_000),
  };
}

function offsetRangeFromSelection(document, selection) {
  return {
    start: document.offsetAt(selection.start),
    end: document.offsetAt(selection.end),
    reversed: selection.anchor.isAfter(selection.active),
  };
}

function vscodeSelectionFromOffsets(document, range, reversed) {
  const start = document.positionAt(range.start);
  const end = document.positionAt(range.end);
  return reversed
    ? new vscode.Selection(end, start)
    : new vscode.Selection(start, end);
}

function restoreSelections(document, snapshot) {
  return snapshot.map(({ anchor, active }) => new vscode.Selection(
    document.positionAt(anchor),
    document.positionAt(active),
  ));
}

function setSelectionsInternally(editor, selections) {
  const proposedSnapshot = selectionSnapshot(editor.document, selections);
  const expected = expectedSelectionChanges.get(editor) ?? new Set();
  expected.add(snapshotSignature(proposedSnapshot));
  expectedSelectionChanges.set(editor, expected);
  editor.selections = selections;

  const actualSnapshot = selectionSnapshot(editor.document, editor.selections);
  expected.add(snapshotSignature(actualSnapshot));
  return actualSnapshot;
}

function expandSelections() {
  const editor = vscode.window.activeTextEditor;
  if (!editor) {
    return;
  }

  const document = editor.document;
  const text = document.getText();
  const options = optionsFromConfiguration();
  const before = selectionSnapshot(document, editor.selections);
  const history = historyFor(editor);
  history.validate(document.uri.toString(), document.version, before);

  const expandedSelections = editor.selections.map((selection) => {
    const current = offsetRangeFromSelection(document, selection);
    const expanded = expandRange(text, current, options);
    return vscodeSelectionFromOffsets(document, expanded, current.reversed);
  });
  const proposed = selectionSnapshot(document, expandedSelections);
  if (snapshotsEqual(before, proposed)) {
    return;
  }

  const after = setSelectionsInternally(editor, expandedSelections);
  history.record(
    document.uri.toString(),
    document.version,
    before,
    after,
  );
}

function shrinkSelections() {
  const editor = vscode.window.activeTextEditor;
  if (!editor) {
    return;
  }

  const history = historyFor(editor);
  const current = selectionSnapshot(editor.document, editor.selections);
  const previous = history.pop(
    editor.document.uri.toString(),
    editor.document.version,
    current,
  );
  if (!previous) {
    return;
  }

  setSelectionsInternally(editor, restoreSelections(editor.document, previous.before));
}

function activate(context) {
  context.subscriptions.push(
    vscode.commands.registerCommand('smartExpand.expand', expandSelections),
    vscode.commands.registerCommand('smartExpand.shrink', shrinkSelections),
    vscode.window.onDidChangeTextEditorSelection((event) => {
      const actual = snapshotSignature(selectionSnapshot(event.textEditor.document, event.selections));
      const expected = expectedSelectionChanges.get(event.textEditor);
      if (expected?.has(actual)) {
        return;
      }
      clearHistory(event.textEditor);
    }),
    vscode.workspace.onDidChangeTextDocument((event) => {
      for (const editor of [...trackedEditors]) {
        if (editor.document.uri.toString() === event.document.uri.toString()) {
          clearHistory(editor);
        }
      }
    }),
    vscode.workspace.onDidCloseTextDocument((document) => {
      for (const editor of [...trackedEditors]) {
        if (editor.document.uri.toString() === document.uri.toString()) {
          clearHistory(editor);
        }
      }
    }),
    vscode.window.onDidChangeActiveTextEditor(clearAllHistory),
  );
}

function deactivate() {
  clearAllHistory();
}

module.exports = {
  activate,
  deactivate,
};

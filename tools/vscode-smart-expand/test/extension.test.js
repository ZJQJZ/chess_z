'use strict';

const assert = require('node:assert/strict');
const Module = require('node:module');
const path = require('node:path');
const test = require('node:test');

class Position {
  constructor(offset) {
    this.offset = offset;
  }

  isAfter(other) {
    return this.offset > other.offset;
  }
}

class Selection {
  constructor(anchor, active) {
    this.anchor = anchor;
    this.active = active;
    this.start = anchor.offset <= active.offset ? anchor : active;
    this.end = anchor.offset <= active.offset ? active : anchor;
  }
}

function makeDocument(text) {
  return {
    version: 1,
    uri: { toString: () => 'file:///test.txt' },
    getText: () => text,
    offsetAt: (position) => position.offset,
    positionAt: (offset) => new Position(offset),
  };
}

function createEvent() {
  const listeners = [];
  return {
    fire(value) {
      for (const listener of listeners) {
        listener(value);
      }
    },
    register(listener) {
      listeners.push(listener);
      return { dispose() {} };
    },
  };
}

function loadExtensionWithMock() {
  const commands = new Map();
  const selectionEvent = createEvent();
  const documentChangeEvent = createEvent();
  const documentCloseEvent = createEvent();
  const activeEditorEvent = createEvent();
  const vscode = {
    Selection,
    commands: {
      registerCommand(command, handler) {
        commands.set(command, handler);
        return { dispose() {} };
      },
    },
    window: {
      activeTextEditor: null,
      onDidChangeTextEditorSelection: selectionEvent.register,
      onDidChangeActiveTextEditor: activeEditorEvent.register,
    },
    workspace: {
      getConfiguration() {
        return { get: (_key, fallback) => fallback };
      },
      onDidChangeTextDocument: documentChangeEvent.register,
      onDidCloseTextDocument: documentCloseEvent.register,
    },
  };

  const extensionPath = path.join(__dirname, '..', 'extension.js');
  delete require.cache[require.resolve(extensionPath)];
  const originalLoad = Module._load;
  Module._load = function mockLoad(request, parent, isMain) {
    if (request === 'vscode') {
      return vscode;
    }
    return originalLoad.call(this, request, parent, isMain);
  };

  let extension;
  try {
    extension = require(extensionPath);
  } finally {
    Module._load = originalLoad;
  }

  extension.activate({ subscriptions: [] });
  return {
    activeEditorEvent,
    commands,
    documentChangeEvent,
    documentCloseEvent,
    extension,
    selectionEvent,
    vscode,
  };
}

function offsets(selection) {
  return {
    anchor: selection.anchor.offset,
    active: selection.active.offset,
  };
}

test('extension commands expand, preserve reverse direction, and shrink', () => {
  const harness = loadExtensionWithMock();
  const document = makeDocument('one (two).');
  const editor = {
    document,
    selections: [new Selection(new Position(6), new Position(6))],
  };
  harness.vscode.window.activeTextEditor = editor;

  harness.commands.get('smartExpand.expand')();
  assert.deepEqual(offsets(editor.selections[0]), { anchor: 5, active: 8 });
  harness.selectionEvent.fire({ textEditor: editor, selections: editor.selections });

  harness.commands.get('smartExpand.expand')();
  assert.deepEqual(offsets(editor.selections[0]), { anchor: 4, active: 9 });
  harness.commands.get('smartExpand.shrink')();
  assert.deepEqual(offsets(editor.selections[0]), { anchor: 5, active: 8 });

  editor.selections = [new Selection(new Position(7), new Position(6))];
  harness.selectionEvent.fire({ textEditor: editor, selections: editor.selections });
  harness.commands.get('smartExpand.expand')();
  assert.deepEqual(offsets(editor.selections[0]), { anchor: 8, active: 5 });

  harness.extension.deactivate();
});

test('extension handles multiple cursors and clears history after external movement', () => {
  const harness = loadExtensionWithMock();
  const document = makeDocument('one two. three four.');
  const editor = {
    document,
    selections: [
      new Selection(new Position(1), new Position(1)),
      new Selection(new Position(18), new Position(16)),
    ],
  };
  harness.vscode.window.activeTextEditor = editor;

  harness.commands.get('smartExpand.expand')();
  assert.deepEqual(editor.selections.map(offsets), [
    { anchor: 0, active: 3 },
    { anchor: 19, active: 15 },
  ]);

  editor.selections = [new Selection(new Position(10), new Position(10))];
  harness.selectionEvent.fire({ textEditor: editor, selections: editor.selections });
  harness.commands.get('smartExpand.shrink')();
  assert.deepEqual(editor.selections.map(offsets), [{ anchor: 10, active: 10 }]);

  harness.activeEditorEvent.fire(null);
  harness.documentChangeEvent.fire({ document });
  harness.documentCloseEvent.fire(document);
  harness.extension.deactivate();
});

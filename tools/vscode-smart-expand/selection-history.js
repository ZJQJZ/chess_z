'use strict';

function snapshotSignature(snapshot) {
  return snapshot.map(({ anchor, active }) => `${anchor}:${active}`).join('|');
}

function snapshotsEqual(left, right) {
  return snapshotSignature(left) === snapshotSignature(right);
}

class SelectionHistory {
  constructor() {
    this.entries = [];
  }

  clear() {
    this.entries.length = 0;
  }

  validate(uri, version, currentSnapshot) {
    if (this.entries.length === 0) {
      return true;
    }

    const latest = this.entries[this.entries.length - 1];
    const valid = latest.uri === uri
      && latest.version === version
      && snapshotsEqual(latest.after, currentSnapshot);
    if (!valid) {
      this.clear();
    }
    return valid;
  }

  record(uri, version, before, after) {
    this.entries.push({ uri, version, before, after });
  }

  pop(uri, version, currentSnapshot) {
    if (!this.validate(uri, version, currentSnapshot)) {
      return null;
    }
    return this.entries.pop() ?? null;
  }

  get length() {
    return this.entries.length;
  }
}

module.exports = {
  SelectionHistory,
  snapshotSignature,
  snapshotsEqual,
};

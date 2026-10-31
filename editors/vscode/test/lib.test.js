'use strict';
const test = require('node:test');
const assert = require('node:assert');
const { parseDiagnostics } = require('../lib');

const lines = ['var x = "a" * 2', 'var y = zzz', 'break'];
const at = (n) => lines[n];

test('maps cdt JSON to zero-based ranges covering the token', () => {
  const json = JSON.stringify([
    { file: 't.coy', line: 2, column: 9, severity: 'error', message: "unknown variable 'zzz'" },
    { file: 't.coy', line: 3, column: 1, severity: 'error', message: "'break' is only allowed inside a loop" }
  ]);
  const r = parseDiagnostics(json, at);
  assert.strictEqual(r.ok, true);
  assert.deepStrictEqual(r.diagnostics[0], { line: 1, startCol: 8, endCol: 11, message: "unknown variable 'zzz'", severity: 'error' });
  assert.deepStrictEqual(r.diagnostics[1], { line: 2, startCol: 0, endCol: 5, message: "'break' is only allowed inside a loop", severity: 'error' });
});

test('an empty array means no diagnostics', () => {
  assert.deepStrictEqual(parseDiagnostics('[]', at), { ok: true, diagnostics: [] });
});

test('garbage output is reported, not thrown', () => {
  assert.strictEqual(parseDiagnostics('not json', at).ok, false);
  assert.strictEqual(parseDiagnostics('{}', at).ok, false);
});

test('entries without a position are skipped; missing line text still gives a visible range', () => {
  const r = parseDiagnostics(JSON.stringify([{ message: 'x' }, { line: 9, column: 1, message: 'far' }]), at);
  assert.strictEqual(r.diagnostics.length, 1);
  assert.strictEqual(r.diagnostics[0].endCol - r.diagnostics[0].startCol, 1);
});

test('entries from other files (imported modules) are dropped when a file filter is given', () => {
  const json = JSON.stringify([
    { file: 'a.coy', line: 1, column: 1, message: 'mine' },
    { file: 'lib/b.coy', line: 2, column: 1, message: 'theirs' }
  ]);
  const r = parseDiagnostics(json, at, (f) => f === 'a.coy');
  assert.deepStrictEqual(r.diagnostics.map((d) => d.message), ['mine']);
});

'use strict';
// Tokenizes sample lines with the real TextMate engine (vscode-textmate + Oniguruma) and checks scopes.
// Skipped when those packages are not installed (they are test-only and not dependencies of the extension).
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const path = require('path');

let vsctm, oniguruma;
try {
  vsctm = require('vscode-textmate');
  oniguruma = require('vscode-oniguruma');
} catch (e) {
  test('grammar (skipped: vscode-textmate not installed)', { skip: true }, () => {});
  return;
}

const wasm = fs.readFileSync(require.resolve('vscode-oniguruma/release/onig.wasm')).buffer;
const lib = oniguruma.loadWASM(wasm).then(() => ({
  createOnigScanner: (s) => new oniguruma.OnigScanner(s),
  createOnigString: (s) => new oniguruma.OnigString(s)
}));
const registry = new vsctm.Registry({
  onigLib: lib,
  loadGrammar: async (scope) =>
    scope === 'source.coyote'
      ? vsctm.parseRawGrammar(fs.readFileSync(path.join(__dirname, '..', 'syntaxes', 'coyote.tmLanguage.json'), 'utf8'), 'g.json')
      : null
});

async function scopesOf(line) {
  const g = await registry.loadGrammar('source.coyote');
  const { tokens } = g.tokenizeLine(line, vsctm.INITIAL);
  return tokens.map((t) => ({ text: line.slice(t.startIndex, t.endIndex), scopes: t.scopes.slice(1) }));
}
const find = (toks, text) => toks.find((t) => t.text === text);

test('comments run to the end of the line', async () => {
  const t = await scopesOf('var x = 1  # note "not a string"');
  assert.deepStrictEqual(find(t, '# note "not a string"').scopes, ['comment.line.number-sign.coyote']);
});

test('hand declarations', async () => {
  const t = await scopesOf('hand:con println(string text)');
  assert.deepStrictEqual(find(t, 'hand').scopes, ['keyword.declaration.hand.coyote']);
  assert.deepStrictEqual(find(t, 'con').scopes, ['entity.name.namespace.coyote']);
  assert.deepStrictEqual(find(t, 'println').scopes, ['entity.name.function.coyote']);
  assert.deepStrictEqual(find(t, 'string').scopes, ['storage.type.coyote']);
});

test('keywords, types, literals and calls', async () => {
  const t = await scopesOf('for (var i = 0; i < 3; i += 1) { if (true) { f(i) } }');
  assert.deepStrictEqual(find(t, 'for').scopes, ['keyword.control.coyote']);
  assert.deepStrictEqual(find(t, 'var').scopes, ['keyword.other.coyote']);
  assert.deepStrictEqual(find(t, '3').scopes, ['constant.numeric.integer.coyote']);
  assert.deepStrictEqual(find(t, 'true').scopes, ['constant.language.boolean.coyote']);
  assert.deepStrictEqual(find(t, 'f').scopes, ['entity.name.function.call.coyote']);
  assert.deepStrictEqual(find(t, '+=').scopes, ['keyword.operator.coyote']);
});

test('strings and escapes', async () => {
  const t = await scopesOf('println("a\\tb \\q")');
  assert.deepStrictEqual(find(t, '\\t').scopes, ['string.quoted.double.coyote', 'constant.character.escape.coyote']);
  assert.deepStrictEqual(find(t, '\\q').scopes, ['string.quoted.double.coyote', 'invalid.illegal.escape.coyote']);
});

test('words reserved by the language design are flagged', async () => {
  const t = await scopesOf('operator Foo');
  assert.deepStrictEqual(find(t, 'operator').scopes, ['invalid.deprecated.reserved.coyote']);
  const c = await scopesOf('class Foo');
  assert.deepStrictEqual(find(c, 'class').scopes, ['keyword.other.coyote']);
});

test('method declarations', async () => {
  const t = await scopesOf('method sumTo(int n) : int {');
  assert.deepStrictEqual(find(t, 'method').scopes, ['keyword.declaration.method.coyote']);
  assert.deepStrictEqual(find(t, 'sumTo').scopes, ['entity.name.function.coyote']);
});

test('new types, null, char and double literals', async () => {
  const t = await scopesOf("double d = 1.5e3\nchar c = 'x'\nint? n = null");
  assert.deepStrictEqual(find(t, 'double').scopes, ['storage.type.coyote']);
  assert.deepStrictEqual(find(t, '1.5e3').scopes, ['constant.numeric.float.coyote']);
  assert.deepStrictEqual(find(t, "'x'").scopes, ['string.quoted.single.coyote']);
  assert.deepStrictEqual(find(t, 'null').scopes, ['constant.language.boolean.coyote']);
});

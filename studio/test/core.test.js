'use strict';
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { findToolchain, exeName, candidateDirs } = require('../src/toolchain');
const { parseDiagnostics } = require('../src/diagnostics');
const { findProjectRoot, readManifest, listDir } = require('../src/project');
const { Settings } = require('../src/settings');
const actions = require('../src/actions');
const { DebugSession } = require('../src/debug');

const BIN = process.env.COYOTE_BIN || path.resolve(__dirname, '..', '..', 'build');
const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), 'studio-core-'));
const haveTools = fs.existsSync(path.join(BIN, exeName('coyote'))) && fs.existsSync(path.join(BIN, exeName('pvm')));
const tc = () => findToolchain({ env: { COYOTE_BIN: BIN, PATH: '' }, execPath: '/nonexistent/x' });

test('exeName adds .exe only on Windows', () => {
  assert.strictEqual(exeName('pvm', 'win32'), 'pvm.exe');
  assert.strictEqual(exeName('pvm', 'linux'), 'pvm');
});

test('candidateDirs order: setting, COYOTE_BIN, installed layout', () => {
  const d = candidateDirs({ settingsDir: 'A', env: { COYOTE_BIN: 'B', PATH: 'P1:P2' }, execPath: path.join('/app', 'studio', 'x.exe'), appDir: '/dev/studio', platform: 'linux' });
  assert.strictEqual(d[0], 'A');
  assert.strictEqual(d[1], 'B');
  assert.strictEqual(d[2], path.resolve('/app/bin'));
  assert.ok(d.includes('P1') && d.includes('P2'));
});

test('findToolchain reports not found for an empty directory', () => {
  const r = findToolchain({ settingsDir: tmp(), env: { PATH: '' }, execPath: '/nonexistent/x', appDir: '/nonexistent/studio' });
  assert.strictEqual(r.dir, null);
  assert.ok(r.missing.includes('coyote'));
});

test('findToolchain needs coyote and pvm together; lists optional tools that are missing', () => {
  const d = tmp();
  fs.writeFileSync(path.join(d, exeName('coyote')), '');
  assert.strictEqual(findToolchain({ settingsDir: d, env: { PATH: '' }, execPath: '/x/y', appDir: '/nonexistent/s' }).dir, null);
  fs.writeFileSync(path.join(d, exeName('pvm')), '');
  const r = findToolchain({ settingsDir: d, env: { PATH: '' }, execPath: '/x/y', appDir: '/nonexistent/s' });
  assert.strictEqual(r.dir, d);
  assert.deepStrictEqual(r.missing.sort(), ['cdt', 'coyote-llvm', 'cpm', 'cpx']);
});

test('parseDiagnostics', () => {
  const r = parseDiagnostics('[{"file":"a.coy","line":2,"column":3,"severity":"error","message":"boom"},{"bad":1}]');
  assert.strictEqual(r.ok, true);
  assert.deepStrictEqual(r.diagnostics, [{ file: 'a.coy', line: 2, column: 3, message: 'boom', severity: 'error' }]);
  assert.strictEqual(parseDiagnostics('nope').ok, false);
  assert.strictEqual(parseDiagnostics('{}').ok, false);
});

test('project helpers', () => {
  const root = tmp();
  fs.mkdirSync(path.join(root, 'src', 'deep'), { recursive: true });
  fs.mkdirSync(path.join(root, '.coyote'));
  fs.writeFileSync(path.join(root, 'packages.json'), '{"schemaVersion":1,"name":"p","entry":"src/main.coy","workflows":{"a":"x","b":"y"}}');
  fs.writeFileSync(path.join(root, 'z.coy'), '');
  assert.strictEqual(findProjectRoot(path.join(root, 'src', 'deep')), root);
  assert.strictEqual(findProjectRoot(path.join(root, 'z.coy')), root);
  const m = readManifest(root);
  assert.deepStrictEqual([m.ok, m.entry, m.workflows], [true, 'src/main.coy', ['a', 'b']]);
  fs.writeFileSync(path.join(root, 'packages.json'), '{broken');
  assert.strictEqual(readManifest(root).ok, false);
  const names = listDir(root).map((x) => x.name);
  assert.deepStrictEqual(names, ['src', 'packages.json', 'z.coy']);  // folders first, .coyote hidden
});

test('settings persist and recent list is bounded and de-duplicated', () => {
  const f = path.join(tmp(), 'sub', 's.json');
  const s = new Settings(f);
  for (let i = 0; i < 12; i++) s.addRecent('/d' + i);
  s.addRecent('/d5');
  const s2 = new Settings(f);
  assert.strictEqual(s2.get('recent').length, 10);
  assert.strictEqual(s2.get('recent')[0], '/d5');
  assert.strictEqual(new Set(s2.get('recent')).size, 10);
  fs.writeFileSync(f, 'not json');
  assert.strictEqual(new Settings(f).get('fontSize'), 14);  // corrupt file falls back to defaults
});

const withTools = haveTools ? test : (name) => test(name, { skip: 'build the toolchain first (COYOTE_BIN)' }, () => {});

withTools('check reports diagnostics, format rewrites, run streams output', async () => {
  const t = tc();
  const d = tmp();
  const bad = path.join(d, 'bad.coy');
  fs.writeFileSync(bad, 'hand:con println(string text)\nprintl("x")\n');
  const c = await actions.check(t, bad, d);
  assert.strictEqual(c.ok, true);
  assert.ok(c.diagnostics.some((x) => x.message.includes("unknown function 'printl'") && x.line === 2));

  const good = path.join(d, 'good.coy');
  fs.writeFileSync(good, 'hand:con println(string text)\nprintln("hi")\n');
  assert.deepStrictEqual((await actions.check(t, good, d)).diagnostics, []);

  const messy = path.join(d, 'messy.coy');
  fs.writeFileSync(messy, 'hand:con println(string text)\nif (true) {\nprintln("x")\n}\n');
  assert.strictEqual((await actions.format(t, messy, d)).ok, true);
  assert.ok(fs.readFileSync(messy, 'utf8').includes('    println("x")'));

  let outText = '';
  const h = actions.runFile(t, good, { cwd: d, onOut: (_p, x) => (outText += x) });
  const r = await h.done;
  assert.strictEqual(r.code, 0);
  assert.strictEqual(outText, 'hi\n');
  const h2 = actions.runFile(t, bad, { cwd: d, onErr: () => {} });
  assert.strictEqual((await h2.done).phase, 'compile');
});

withTools('showMl, native build (or clear refusal), versions', async () => {
  const t = tc();
  const d = tmp();
  const f = path.join(d, 'p.coy');
  fs.writeFileSync(f, 'hand:con println(string text)\nprintln("n")\n');
  const ml = await actions.showMl(t, f, d);
  assert.ok(ml.ok && ml.text.includes('func $main'));
  if (t.tools['coyote-llvm']) {
    const exe = path.join(d, exeName('p'));
    const r = await actions.buildNative(t, f, exe, d);
    if (r.ok) assert.ok(fs.existsSync(exe));  // clang may be missing: then a clear message, not a crash
    else assert.ok(r.output.length > 0);
  }
  const v = await actions.versions(t);
  assert.ok(v.coyote && v.pvm);
});

withTools('DebugSession: breakpoints, stack, variables, stepping, exit', async () => {
  const t = tc();
  const d = tmp();
  const f = path.join(d, 'd.coy');
  fs.writeFileSync(f, 'hand:con println(string text)\nmethod sq(int n): int {\n    int r = n * n\n    return r\n}\nint a = sq(4)\nprintln("a " + a)\n');
  const s = new DebugSession(t);
  let out = '';
  s.on('output', (x) => (out += x));
  const stops = [];
  s.on('stopped', (e) => stops.push(e));
  const exited = new Promise((r) => s.on('exited', r));
  await s.start(f, { cwd: d, breakpoints: [{ file: f, line: 4 }] });
  const waitStops = async (n) => { for (let i = 0; i < 100 && stops.length < n; i++) await new Promise((r) => setTimeout(r, 50)); assert.ok(stops.length >= n, 'stop ' + n); };
  await waitStops(1);
  assert.strictEqual(stops[0].reason, 'entry');
  s.resume();
  await waitStops(2);
  assert.strictEqual(stops[1].reason, 'breakpoint');
  assert.strictEqual(stops[1].line, 4);
  const st = await s.stack();
  assert.deepStrictEqual(st.map((x) => x.function), ['sq', '$main']);
  const vars = await s.variables(0);
  assert.deepStrictEqual(vars.map((v) => [v.name, v.value]), [['n', '4'], ['r', '16']]);
  const outer = await s.variables(1);
  assert.ok(outer.some((v) => v.name === 'a'));
  s.stepOut();
  await waitStops(3);
  assert.strictEqual(stops[2].reason, 'step');
  s.resume();
  const e = await exited;
  assert.strictEqual(e.code, 0);
  assert.strictEqual(out, 'a 16\n');
});

withTools('DebugSession: compile errors are reported, stop() ends a paused program', async () => {
  const t = tc();
  const d = tmp();
  const bad = path.join(d, 'bad.coy');
  fs.writeFileSync(bad, 'printl("x")\n');
  const s = new DebugSession(t);
  await assert.rejects(s.start(bad, { cwd: d }), (e) => /unknown function/.test(e.compileOutput));

  const ok = path.join(d, 'ok.coy');
  fs.writeFileSync(ok, 'hand:con println(string text)\nprintln("x")\n');
  const s2 = new DebugSession(t);
  const stopped = new Promise((r) => s2.on('stopped', r));
  const exited = new Promise((r) => s2.on('exited', r));
  await s2.start(ok, { cwd: d });
  await stopped;
  s2.stop();
  await exited;
});

withTools('packLibrary builds out/<name>.lib and can install it for the user; the lib is usable', async () => {
  const t = tc();
  const d = tmp();
  const proj = path.join(d, 'tools');
  fs.mkdirSync(path.join(proj, 'src'), { recursive: true });
  fs.writeFileSync(path.join(proj, 'packages.json'), '{ "schemaVersion": 1, "name": "Tools", "version": "1.0.0", "type": "library" }\n');
  fs.writeFileSync(path.join(proj, 'src', 'tools.coy'), 'export method twice(int x): int { return x * 2 }\n');
  const r = await actions.packLibrary(t, proj, false);
  assert.strictEqual(r.ok, true, r.output);
  assert.strictEqual(r.file, path.join(proj, 'out', 'tools.lib'));
  assert.ok(fs.existsSync(r.file));

  const app = path.join(d, 'app.coy');
  fs.writeFileSync(app, 'use "./tools/out/tools.lib"\nhand:con println(string text)\nprintln("" + twice(21))\n');
  const run = await require('../src/runner').capture(t.tools.coyote, ['run', app], { cwd: d });
  assert.strictEqual(run.stdout.trim(), '42');

  fs.writeFileSync(path.join(proj, 'src', 'tools.coy'), 'export method twice(int x): int { return "no" }\n');
  const bad = await actions.packLibrary(t, proj, false);
  assert.strictEqual(bad.ok, false);
  assert.match(bad.output, /tools\.coy:1/);
});

// ---- language index (renderer/langindex.js) ----
const L = require('../renderer/langindex.js');
test('langindex: indexes classes, members, docs and hand declarations', () => {
  const ix = L.indexSource('use "Standard"\nhand:con println(string text)\n# A counter.\nexport class Counter {\n    # Adds one.\n    public static method Bump(int by): int { return by + 1 }\n    property int Count\n    string label\n}\nmethod helper(): int { return 1 }\n');
  assert.deepStrictEqual(ix.uses.map((u) => u.name), ['Standard']);
  assert.strictEqual(ix.hands[0].name, 'println');
  const c = ix.classes[0];
  assert.strictEqual(c.name, 'Counter');
  assert.ok(c.exported);
  assert.strictEqual(c.doc, 'A counter.');
  const bump = c.members.find((m) => m.name === 'Bump');
  assert.ok(bump.static);
  assert.strictEqual(bump.doc, 'Adds one.');
  assert.strictEqual(L.signatureOf(bump), 'static method Bump(int by): int');
  assert.deepStrictEqual(c.members.map((m) => m.kind), ['method', 'property', 'field']);
  assert.strictEqual(ix.methods[0].name, 'helper');
});
test('langindex: parses .lib bundles with requires and multibyte content', () => {
  const body = '# ünï\nexport class A { }\n';
  const bytes = Buffer.byteLength(body);
  const lib = L.parseLib(`coyote-lib 1\nname Demo\nversion 1.0.0\nrequires Other\nfile a.coy ${bytes}\n${body}`);
  assert.strictEqual(lib.error, null);
  assert.deepStrictEqual(lib.requires, ['Other']);
  assert.strictEqual(lib.files[0].content, body);
});
test('langindex: use insertion goes after the last use or the header comment', () => {
  assert.deepStrictEqual(L.useInsertion('use "A"\nx\n', 'B'), { line: 2, text: 'use "B"\n' });
  assert.deepStrictEqual(L.useInsertion('# c\nhand:con p(string t)\n', 'B'), { line: 2, text: 'use "B"\n\n' });
  assert.ok(L.hasUse('use "Standard.IO"\n', 'standard.io'));
});
test('langindex: umbrella library makes required libraries visible', () => {
  const mk = (name, requires, src) => ({ lib: { name, version: '1', requires, files: src ? [{ name: 'a.coy', content: src }] : [], error: null }, path: name });
  const libs = L.buildLibIndex([mk('Standard', ['Standard.IO'], ''), mk('Standard.IO', [], 'export class Shell {\n    public static method Hi() { }\n}\n')]);
  const ctx = L.analyze('use "Standard"\nShell.Hi()\n', { libs });
  assert.ok(ctx.classes.has('Shell'));
  assert.deepStrictEqual(L.typeOfExpr(ctx, 'Shell', 2), { staticClass: 'Shell' });
  const ctx2 = L.analyze('Shell.Hi()\n', { libs });
  assert.ok(!ctx2.classes.has('Shell'));
});
test('langindex: resolves variable and chained member types', () => {
  const ctx = L.analyze('class P {\n    string name\n}\nclass Q {\n    P owner\n}\nvar q = new Q()\narray<P> ps = new array<P>(1)\n', {});
  assert.deepStrictEqual(L.typeOfExpr(ctx, 'q', 8), { type: 'Q' });
  assert.deepStrictEqual(L.typeOfExpr(ctx, 'q.owner', 8), { type: 'P' });
  assert.deepStrictEqual(L.typeOfExpr(ctx, 'ps[0].name', 8), { type: 'string' });
  assert.deepStrictEqual(L.typeOfExpr(ctx, 'ps', 8), { type: 'array<P>' });
});

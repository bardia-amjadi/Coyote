'use strict';
// The things the IDE does with the toolchain: check, format, run, build, native build, packages.
const fs = require('fs');
const os = require('os');
const path = require('path');
const runner = require('./runner');
const { parseDiagnostics } = require('./diagnostics');

function need(tc, name) {
  if (!tc.tools[name]) throw new Error(`The Coyote tool '${name}' was not found. Install Coyote (or set the toolchain folder in Settings).`);
  return tc.tools[name];
}

async function check(tc, file, cwd) {
  const r = await runner.capture(need(tc, 'cdt'), ['check', '--json', file], { cwd });
  if (r.error) return { ok: false, error: r.error, diagnostics: [] };
  const parsed = parseDiagnostics(r.stdout);
  if (!parsed.ok) return { ...parsed, error: parsed.error + (r.stderr ? ': ' + r.stderr.trim() : '') };
  return { ok: true, diagnostics: parsed.diagnostics };
}

// Formats in place; resolves { ok, message }.
async function format(tc, file, cwd) {
  const r = await runner.capture(need(tc, 'cdt'), ['fmt', '--write', file], { cwd });
  return { ok: r.code === 0, message: (r.stderr || r.stdout || r.error || '').trim() };
}

function tempDir() { return fs.mkdtempSync(path.join(os.tmpdir(), 'coyote-studio-')); }

// Compiles `file` to a temporary .ml and runs it on the PVM. Returns a handle like runner.start.
// Output callbacks receive ('compile'|'run', text) so the UI can tell the phases apart.
function runFile(tc, file, { cwd, onOut, onErr } = {}) {
  const tmp = tempDir();
  const ml = path.join(tmp, 'program.ml');
  let current = null;
  let killed = false;
  const done = (async () => {
    const b = await runner.capture(need(tc, 'coyote'), ['build', file, '-o', ml], { cwd });
    if (b.code !== 0) {
      onErr && onErr('compile', b.stderr || b.stdout || b.error || '');
      return { code: b.code, phase: 'compile' };
    }
    if (killed) return { code: 130, phase: 'run' };
    current = runner.start(need(tc, 'pvm'), [ml], { cwd, onStdout: (d) => onOut && onOut('run', d), onStderr: (d) => onErr && onErr('run', d) });
    const r = await current.done;
    if (r.error) onErr && onErr('run', r.error + '\n');
    return { code: r.code, phase: 'run' };
  })().finally(() => { try { fs.rmSync(tmp, { recursive: true, force: true }); } catch {} });
  return {
    done,
    kill() { killed = true; if (current) current.kill(); },
    write(t) { if (current) current.write(t); },
  };
}

// Runs a cpx workflow of the project in `root`.
function runWorkflow(tc, root, workflow, { onOut, onErr } = {}) {
  return runner.start(need(tc, 'cpx'), ['-C', root, workflow], { cwd: root, onStdout: (d) => onOut && onOut(d), onStderr: (d) => onErr && onErr(d) });
}

// Compiles to ML and then to a native executable with coyote-llvm (a subset of the language).
async function buildNative(tc, file, outExe, cwd) {
  const tmp = tempDir();
  try {
    const ml = path.join(tmp, 'program.ml');
    const b = await runner.capture(need(tc, 'coyote'), ['build', file, '-o', ml], { cwd });
    if (b.code !== 0) return { ok: false, phase: 'compile', output: (b.stderr || b.stdout || b.error || '').trim() };
    const n = await runner.capture(need(tc, 'coyote-llvm'), ['-o', outExe, ml], { cwd });
    return { ok: n.code === 0, phase: 'native', output: (n.stderr || n.stdout || n.error || '').trim() };
  } finally {
    try { fs.rmSync(tmp, { recursive: true, force: true }); } catch {}
  }
}

// Compiles to ML text (for the "show ML" view).
async function showMl(tc, file, cwd) {
  const r = await runner.capture(need(tc, 'coyote'), ['build', file, '-o', '-'], { cwd });
  return { ok: r.code === 0, text: r.code === 0 ? r.stdout : (r.stderr || r.error || '').trim() };
}

// Packs a library project (packages.json with "type": "library") into out/<name>.lib; with `install`
// the file is also copied to the current user's library folder (~/coyote/libs), where `use "Name"` finds it.
async function packLibrary(tc, root, install) {
  let name;
  try { name = String(JSON.parse(fs.readFileSync(path.join(root, 'packages.json'), 'utf8')).name || ''); } catch (e) { return { ok: false, output: 'cannot read packages.json: ' + e.message }; }
  const outFile = path.join(root, 'out', name.toLowerCase() + '.lib');
  fs.mkdirSync(path.dirname(outFile), { recursive: true });
  const r = await runner.capture(need(tc, 'coyote'), ['lib', 'pack', root, '-o', outFile], { cwd: root });
  const output = (r.stdout + r.stderr || r.error || '').trim();
  if (r.code !== 0) return { ok: false, output };
  if (!install) return { ok: true, output, file: outFile };
  const dest = path.join(userLibsDir(), path.basename(outFile));
  fs.mkdirSync(path.dirname(dest), { recursive: true });
  fs.copyFileSync(outFile, dest);
  return { ok: true, output, file: outFile, installedTo: dest };
}

function userLibsDir() { return path.join(os.homedir(), 'coyote', 'libs'); }

function cpm(tc, root, args, { onOut, onErr } = {}) {
  return runner.start(need(tc, 'cpm'), ['-C', root, ...args], { cwd: root, onStdout: (d) => onOut && onOut(d), onStderr: (d) => onErr && onErr(d) });
}

async function doctor(tc) {
  const r = await runner.capture(need(tc, 'cdt'), ['doctor'], {});
  return { ok: r.code === 0, text: (r.stdout + r.stderr).trim() };
}

async function versions(tc) {
  const out = {};
  for (const t of ['coyote', 'pvm', 'cpm', 'cpx', 'cdt', 'coyote-llvm']) {
    if (!tc.tools[t]) { out[t] = null; continue; }
    const r = await runner.capture(tc.tools[t], ['--version'], {});
    out[t] = r.code === 0 ? r.stdout.trim() : '?';
  }
  return out;
}

module.exports = { check, format, runFile, runWorkflow, buildNative, showMl, packLibrary, userLibsDir, cpm, doctor, versions };

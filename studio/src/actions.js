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
    current = runner.start(need(tc, 'pvm'), ['--allow-all', '--unbuffered', ml], { cwd, onStdout: (d) => onOut && onOut('run', d), onStderr: (d) => onErr && onErr('run', d) });
    const r = await current.done;
    if (r.error) onErr && onErr('run', r.error + '\n');
    return { code: r.code, phase: 'run' };
  })().finally(() => { try { fs.rmSync(tmp, { recursive: true, force: true }); } catch {} });
  return {
    done,
    kill() { killed = true; if (current) current.kill(); },
    write(t) { if (current) current.write(t); },
    closeStdin() { if (current) current.closeStdin(); },
  };
}

// Runs a cpx workflow of the project in `root`.
function runWorkflow(tc, root, workflow, { onOut, onErr } = {}) {
  return runner.start(need(tc, 'cpx'), ['-C', root, workflow], { cwd: root, onStdout: (d) => onOut && onOut(d), onStderr: (d) => onErr && onErr(d) });
}

// Compiles to ML text (for the "show ML" view).
async function showMl(tc, file, cwd) {
  const r = await runner.capture(need(tc, 'coyote'), ['build', file, '-o', '-'], { cwd });
  return { ok: r.code === 0, text: r.code === 0 ? r.stdout : (r.stderr || r.error || '').trim() };
}

// Packs a library project (project file with "type": "library") into out/<name>.lib; with `install`
// the file is also copied to the current user's library folder (~/coyote/libs), where `use "Name"` finds it.
async function packLibrary(tc, root, install) {
  let name;
  const info = require('./project').readManifest(root);
  if (!info.ok) return { ok: false, output: 'cannot read the project file: ' + info.error };
  name = String(info.manifest.name || '');
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

// Checks unsaved text: writes it next to `file` (so relative `use`/`import` paths still resolve) under a hidden
// name, checks that, and maps the diagnostics back to `file`.
async function checkText(tc, file, text, cwd) {
  const dir = path.dirname(file);
  const tmpName = `.~studio-${process.pid}-${path.basename(file)}`;
  const tmp = path.join(dir, tmpName);
  try {
    fs.writeFileSync(tmp, text, 'utf8');
  } catch (e) { return { ok: false, error: e.message, diagnostics: [] }; }
  try {
    const r = await check(tc, tmp, cwd);
    if (!r.ok) return r;
    const fix = (m) => m.split(tmpName).join(path.basename(file));
    const diagnostics = r.diagnostics.map((d) => (path.basename(d.file || '') === tmpName ? { ...d, file, message: fix(d.message) } : { ...d, message: fix(d.message) }));
    return { ok: true, diagnostics };
  } finally {
    try { fs.unlinkSync(tmp); } catch { /* already gone */ }
  }
}

// Formats text without touching any file of the user's: returns { ok, text }.
async function formatText(tc, text) {
  const tmp = tempDir();
  try {
    const f = path.join(tmp, 'format.coy');
    fs.writeFileSync(f, text, 'utf8');
    const r = await runner.capture(need(tc, 'cdt'), ['fmt', '--write', f], { cwd: tmp });
    if (r.code !== 0) return { ok: false, message: (r.stderr || r.stdout || r.error || '').trim() };
    return { ok: true, text: fs.readFileSync(f, 'utf8') };
  } finally {
    try { fs.rmSync(tmp, { recursive: true, force: true }); } catch { /* temp dir already gone */ }
  }
}

// Every directory `use "Name"` searches (see docs/libraries.md), most specific first.
function libDirs(tc, projectRoot) {
  const dirs = [];
  if (process.env.COYOTE_LIBS) dirs.push(...process.env.COYOTE_LIBS.split(path.delimiter).filter(Boolean));
  if (projectRoot) dirs.push(path.join(projectRoot, '.coyote', 'libs'));
  if (tc.dir) { dirs.push(path.join(tc.dir, 'libs')); dirs.push(path.join(path.dirname(tc.dir), 'libs')); }
  dirs.push(userLibsDir());
  return [...new Set(dirs)];
}

// All installed .lib files: [{ file, text }]. The first folder that has a name wins, as in the compiler.
function readLibs(tc, projectRoot) {
  const seen = new Set();
  const out = [];
  for (const d of libDirs(tc, projectRoot)) {
    let names;
    try { names = fs.readdirSync(d); } catch { continue; }
    for (const n of names.sort()) {
      if (!/\.lib$/i.test(n) || seen.has(n.toLowerCase())) continue;
      try { out.push({ file: path.join(d, n), text: fs.readFileSync(path.join(d, n), 'utf8') }); seen.add(n.toLowerCase()); } catch { /* unreadable: skip */ }
    }
  }
  return { dirs: libDirs(tc, projectRoot), libs: out };
}

module.exports = { formatText, checkText, libDirs, readLibs, check, format, runFile, runWorkflow, showMl, packLibrary, userLibsDir, cpm, doctor, versions };

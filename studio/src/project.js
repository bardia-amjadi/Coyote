'use strict';
// Project helpers: the project file (<Name>.cyproj or packages.json), build settings, directory listing.
const fs = require('fs');
const path = require('path');

// The project file of a folder: packages.json wins, otherwise the first *.cyproj by name. null when there is none.
function projectFileIn(dir) {
  try {
    if (fs.existsSync(path.join(dir, 'packages.json'))) return path.join(dir, 'packages.json');
    const c = fs.readdirSync(dir).filter((n) => n.toLowerCase().endsWith('.cyproj')).sort();
    return c.length ? path.join(dir, c[0]) : null;
  } catch { return null; }
}

// `startPath` may be a folder, a file inside a project, or the project file itself.
function findProjectRoot(startPath) {
  let dir = fs.existsSync(startPath) && fs.statSync(startPath).isDirectory() ? startPath : path.dirname(startPath);
  dir = path.resolve(dir);
  for (;;) {
    if (projectFileIn(dir)) return dir;
    const up = path.dirname(dir);
    if (up === dir) return null;
    dir = up;
  }
}

// The effective build settings of a manifest (defaults filled in), see docs/projects.md.
function settingsOf(m) {
  const out = m.output && typeof m.output === 'object' ? m.output : null;
  const winExe = process.platform === 'win32';
  const kind = out && out.kind === 'ml' ? 'ml' : 'native';
  const name = out && out.name ? String(out.name) : String(m.name || 'app');
  const dir = out && out.directory ? String(out.directory) : 'bin';
  return {
    hasOutput: !!out, directory: dir, name, kind, mode: out && out.mode ? String(out.mode) : 'auto', keepUnused: !!(out && out.keepUnused),
    file: path.join(dir, name + (kind === 'ml' ? '.ml' : winExe ? '.exe' : '')),
    args: m.run && Array.isArray(m.run.args) ? m.run.args.map(String) : [],
  };
}

function readManifest(root) {
  const file = projectFileIn(root);
  if (!file) return { ok: false, error: 'no project file', entry: 'app.coy', workflows: [] };
  try {
    const m = JSON.parse(fs.readFileSync(file, 'utf8'));
    return { ok: true, file, kind: file.toLowerCase().endsWith('.cyproj') ? 'cyproj' : 'packages', manifest: m,
      entry: typeof m.entry === 'string' ? m.entry : 'app.coy', workflows: Object.keys(m.workflows || {}), settings: settingsOf(m) };
  } catch (e) {
    return { ok: false, file, error: e.message, entry: 'app.coy', workflows: [] };
  }
}

const ALLOWED_OUT = ['directory', 'name', 'kind', 'mode', 'keepUnused'];
// Writes edited properties into the project file, keeping every key Studio does not manage. Returns the new manifest info.
function saveProject(root, patch) {
  const file = projectFileIn(root);
  if (!file) throw new Error('this folder has no project file');
  const m = JSON.parse(fs.readFileSync(file, 'utf8'));
  for (const k of ['name', 'version', 'type', 'entry', 'description']) if (typeof patch[k] === 'string') { if (patch[k] === '' && (k === 'description' || k === 'version')) delete m[k]; else m[k] = patch[k]; }
  if (Array.isArray(patch.requires)) { if (patch.requires.length) m.requires = patch.requires.map(String); else delete m.requires; }
  if (patch.output && typeof patch.output === 'object') {
    const o = { ...(m.output || {}) };
    for (const k of ALLOWED_OUT) if (k in patch.output) o[k] = patch.output[k];
    m.output = o;
  }
  if (patch.run && Array.isArray(patch.run.args)) m.run = { ...(m.run || {}), args: patch.run.args.map(String) };
  fs.writeFileSync(file, JSON.stringify(m, null, 2) + '\n', 'utf8');
  return readManifest(root);
}

// A .cyproj for a folder that has none (an app whose entry is `entry`, relative to the folder).
function createProjectFile(dir, name, entry) {
  const file = path.join(dir, name + '.cyproj');
  if (fs.existsSync(file)) throw new Error(name + '.cyproj already exists');
  const m = { schemaVersion: 1, name, version: '1.0.0', type: 'app', entry, description: '',
    output: { directory: 'bin', name, kind: 'native', mode: 'auto', keepUnused: false }, run: { args: [] } };
  fs.writeFileSync(file, JSON.stringify(m, null, 2) + '\n', 'utf8');
  return file;
}

// Removes build results: the hidden .coyote/out folder and the configured output file / library file.
function cleanProject(root) {
  const info = readManifest(root);
  const removed = [];
  const rm = (p) => { if (fs.existsSync(p)) { fs.rmSync(p, { recursive: true, force: true }); removed.push(p); } };
  rm(path.join(root, '.coyote', 'out'));
  if (info.ok) {
    const m = info.manifest;
    if (m.type === 'library') rm(path.join(root, 'out', String(m.name || '').toLowerCase() + '.lib'));
    else rm(path.join(root, info.settings.file));
  }
  return removed;
}

const HIDDEN = new Set(['.git', 'node_modules', '.coyote', '.vs', '.idea']);

// One directory level, folders first. Hidden build folders are skipped.
function listDir(dir) {
  let names;
  try { names = fs.readdirSync(dir, { withFileTypes: true }); } catch { return []; }
  return names
    .filter((d) => !HIDDEN.has(d.name) && !d.name.startsWith('.~studio-'))
    .map((d) => ({ name: d.name, path: path.join(dir, d.name), dir: d.isDirectory() }))
    .sort((a, b) => (a.dir === b.dir ? a.name.localeCompare(b.name) : a.dir ? -1 : 1));
}

module.exports = { findProjectRoot, projectFileIn, readManifest, saveProject, createProjectFile, cleanProject, settingsOf, listDir };

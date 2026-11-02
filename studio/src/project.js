'use strict';
// Project helpers: packages.json lookup, workflows, directory listing.
const fs = require('fs');
const path = require('path');

function findProjectRoot(startPath) {
  let dir = fs.existsSync(startPath) && fs.statSync(startPath).isDirectory() ? startPath : path.dirname(startPath);
  dir = path.resolve(dir);
  for (;;) {
    if (fs.existsSync(path.join(dir, 'packages.json'))) return dir;
    const up = path.dirname(dir);
    if (up === dir) return null;
    dir = up;
  }
}

function readManifest(root) {
  const file = path.join(root, 'packages.json');
  try {
    const m = JSON.parse(fs.readFileSync(file, 'utf8'));
    return { ok: true, manifest: m, entry: typeof m.entry === 'string' ? m.entry : 'app.coy', workflows: Object.keys(m.workflows || {}) };
  } catch (e) {
    return { ok: false, error: e.message, entry: 'app.coy', workflows: [] };
  }
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

module.exports = { findProjectRoot, readManifest, listDir };

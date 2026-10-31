'use strict';
// Locates the Coyote toolchain (coyote, pvm, cpm, cpx, cdt, coyote-llvm) and runs its programs.
const fs = require('fs');
const path = require('path');

const TOOLS = ['coyote', 'pvm', 'cpm', 'cpx', 'cdt', 'coyote-llvm'];
const REQUIRED = ['coyote', 'pvm'];

function exeName(name, platform = process.platform) {
  return platform === 'win32' ? name + '.exe' : name;
}

function isFile(p) {
  try { return fs.statSync(p).isFile(); } catch { return false; }
}

// Directories searched, in order: the user's setting, COYOTE_BIN, the installed layout
// ({app}\studio\CoyoteStudio.exe next to {app}\bin), a development build next to the repository, and PATH.
function candidateDirs({ settingsDir, env = process.env, execPath = process.execPath, appDir = path.resolve(__dirname, '..'), platform = process.platform } = {}) {
  const dirs = [];
  if (settingsDir) dirs.push(settingsDir);
  if (env.COYOTE_BIN) dirs.push(env.COYOTE_BIN);
  dirs.push(path.resolve(path.dirname(execPath), '..', 'bin'));
  dirs.push(path.resolve(appDir, '..', 'bin'));
  dirs.push(path.resolve(appDir, '..', 'build'));
  const sep = platform === 'win32' ? ';' : ':';
  for (const d of (env.PATH || env.Path || '').split(sep)) if (d) dirs.push(d);
  return dirs;
}

// Returns { dir, tools: {name: absolute path}, missing: [names] }. `dir` is null when even the required tools were not found together.
function findToolchain(opts = {}) {
  const platform = opts.platform || process.platform;
  for (const dir of candidateDirs(opts)) {
    if (REQUIRED.every((t) => isFile(path.join(dir, exeName(t, platform))))) {
      const tools = {};
      const missing = [];
      for (const t of TOOLS) {
        const p = path.join(dir, exeName(t, platform));
        if (isFile(p)) tools[t] = p; else missing.push(t);
      }
      return { dir, tools, missing };
    }
  }
  return { dir: null, tools: {}, missing: TOOLS.slice() };
}

module.exports = { TOOLS, REQUIRED, exeName, candidateDirs, findToolchain };

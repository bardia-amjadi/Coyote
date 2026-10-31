'use strict';
// Converts the JSON printed by `cdt check --json` into editor markers (1-based line/column).
const path = require('path');

function parseDiagnostics(stdout) {
  let items;
  try { items = JSON.parse(stdout); } catch { return { ok: false, error: 'cdt printed output that is not JSON', diagnostics: [] }; }
  if (!Array.isArray(items)) return { ok: false, error: 'cdt output is not an array', diagnostics: [] };
  const diagnostics = [];
  for (const d of items) {
    if (!d || typeof d.line !== 'number' || typeof d.column !== 'number' || typeof d.message !== 'string') continue;
    diagnostics.push({ file: typeof d.file === 'string' ? d.file : '', line: d.line, column: d.column, message: d.message, severity: d.severity || 'error' });
  }
  return { ok: true, diagnostics };
}

// True when a diagnostic's file is `target` (diagnostics are printed relative to the cwd of the run).
function sameFile(diagFile, target, cwd) {
  if (!diagFile) return true;
  const a = path.resolve(cwd || '.', diagFile);
  return a === path.resolve(target);
}

module.exports = { parseDiagnostics, sameFile };

'use strict';
// Project-wide file listing and text search (Find in Files, Quick Open).
const fs = require('fs');
const path = require('path');

const SKIP_DIRS = new Set(['.git', 'node_modules', '.coyote', '.vs', '.idea', 'out', 'dist', 'build', '.vscode']);
const MAX_FILE_BYTES = 1024 * 1024;

function isHiddenTemp(name) { return name.startsWith('.~studio-'); }

// Relative-to-nothing absolute paths of every file below `root` (bounded).
function listFiles(root, { limit = 5000 } = {}) {
  const out = [];
  const stack = [root];
  while (stack.length && out.length < limit) {
    const dir = stack.pop();
    let entries;
    try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch { continue; }
    entries.sort((a, b) => a.name.localeCompare(b.name));
    for (const e of entries) {
      if (isHiddenTemp(e.name)) continue;
      const full = path.join(dir, e.name);
      if (e.isDirectory()) { if (!SKIP_DIRS.has(e.name)) stack.push(full); } else if (e.isFile()) { out.push(full); if (out.length >= limit) break; }
    }
  }
  return out;
}

function looksBinary(buf) {
  const n = Math.min(buf.length, 4096);
  for (let i = 0; i < n; i++) if (buf[i] === 0) return true;
  return false;
}

// Returns { results: [{file, line, column, text}], truncated }.
function searchText(root, query, { caseSensitive = false, regex = false, wholeWord = false, include = '', limit = 1000 } = {}) {
  if (!query) return { results: [], truncated: false };
  let re;
  try {
    let src = regex ? query : query.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    if (wholeWord) src = `\\b(?:${src})\\b`;
    re = new RegExp(src, caseSensitive ? 'g' : 'gi');
  } catch (e) { return { results: [], truncated: false, error: 'invalid pattern: ' + e.message }; }
  const exts = include.split(/[,\s]+/).filter(Boolean).map((x) => x.replace(/^\*?\.?/, '.').toLowerCase());
  const results = [];
  let truncated = false;
  for (const file of listFiles(root, { limit: 20000 })) {
    if (exts.length && !exts.includes(path.extname(file).toLowerCase())) continue;
    let buf;
    try { const st = fs.statSync(file); if (st.size > MAX_FILE_BYTES) continue; buf = fs.readFileSync(file); } catch { continue; }
    if (looksBinary(buf)) continue;
    const lines = buf.toString('utf8').split(/\r?\n/);
    for (let i = 0; i < lines.length; i++) {
      re.lastIndex = 0;
      const m = re.exec(lines[i]);
      if (m) {
        results.push({ file, line: i + 1, column: m.index + 1, length: m[0].length, text: lines[i].slice(0, 300) });
        if (results.length >= limit) { truncated = true; return { results, truncated }; }
      }
    }
  }
  return { results, truncated };
}

module.exports = { listFiles, searchText, isHiddenTemp };

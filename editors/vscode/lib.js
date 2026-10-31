// Pure helpers for the extension (no dependency on the VS Code API, so they can be tested with node).
'use strict';

// Converts the JSON printed by `cdt check --json` into 0-based ranges.
// `lineText(n)` returns the text of 0-based line n (or undefined) so the range can cover the token.
// `isCurrentFile(file)` (optional) drops entries that belong to other files (errors in imported modules).
function parseDiagnostics(stdout, lineText, isCurrentFile) {
  let items;
  try {
    items = JSON.parse(stdout);
  } catch (e) {
    return { ok: false, error: 'cdt printed output that is not JSON' };
  }
  if (!Array.isArray(items)) return { ok: false, error: 'cdt output is not an array' };
  const out = [];
  for (const d of items) {
    if (!d || typeof d.line !== 'number' || typeof d.column !== 'number' || typeof d.message !== 'string') continue;
    if (isCurrentFile && typeof d.file === 'string' && !isCurrentFile(d.file)) continue;
    const line = Math.max(0, d.line - 1);
    const startCol = Math.max(0, d.column - 1);
    const text = (lineText && lineText(line)) || '';
    // Extend the range over the identifier/number/operator that starts at the reported column.
    let end = startCol;
    const rest = text.slice(startCol);
    const m = /^([A-Za-z_][A-Za-z0-9_]*|[0-9]+|"(?:[^"\\]|\\.)*"?|.)/.exec(rest);
    end = startCol + (m ? m[0].length : 1);
    out.push({ line, startCol, endCol: Math.max(end, startCol + 1), message: d.message, severity: d.severity || 'error' });
  }
  return { ok: true, diagnostics: out };
}

module.exports = { parseDiagnostics };

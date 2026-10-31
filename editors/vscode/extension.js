'use strict';

// Coyote language support: diagnostics and formatting through the `cdt` developer tool.
// All language intelligence lives in cdt; this file only connects it to the editor.
const vscode = require('vscode');
const cp = require('child_process');
const path = require('path');
const { parseDiagnostics } = require('./lib');

const MAX_BYTES = 4 * 1024 * 1024;

function cdtPath() {
  return vscode.workspace.getConfiguration('coyote').get('cdtPath') || 'cdt';
}

function runCdt(args, input) {
  return new Promise((resolve) => {
    const child = cp.spawn(cdtPath(), args, { shell: false });
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (d) => { stdout += d; });
    child.stderr.on('data', (d) => { stderr += d; });
    child.on('error', (e) => resolve({ code: -1, stdout, stderr: String(e.message) }));
    child.on('close', (code) => resolve({ code, stdout, stderr }));
    if (input !== undefined) child.stdin.end(input);
    else child.stdin.end();
  });
}

async function checkDocument(doc, collection, output) {
  if (doc.languageId !== 'coyote' || doc.uri.scheme !== 'file') return;
  if (!vscode.workspace.getConfiguration('coyote').get('diagnostics.enable')) {
    collection.delete(doc.uri);
    return;
  }
  // cdt reads files, so the saved file is checked (diagnostics refresh on open and on save).
  const res = await runCdt(['check', '--json', doc.uri.fsPath]);
  if (res.code === -1) {
    output.appendLine(`cdt could not be started: ${res.stderr}. Set "coyote.cdtPath".`);
    collection.delete(doc.uri);
    return;
  }
  if (res.code !== 0 && res.code !== 1) {
    output.appendLine(`cdt check failed (exit code ${res.code}): ${res.stderr}`);
    return;
  }
  const parsed = parseDiagnostics(res.stdout, (n) => (n < doc.lineCount ? doc.lineAt(n).text : undefined),
    (f) => path.resolve(f) === path.resolve(doc.uri.fsPath));
  if (!parsed.ok) {
    output.appendLine(parsed.error);
    return;
  }
  collection.set(
    doc.uri,
    parsed.diagnostics.map((d) => {
      const diag = new vscode.Diagnostic(
        new vscode.Range(d.line, d.startCol, d.line, d.endCol),
        d.message,
        vscode.DiagnosticSeverity.Error
      );
      diag.source = 'cdt';
      return diag;
    })
  );
}

function activate(context) {
  const collection = vscode.languages.createDiagnosticCollection('coyote');
  const output = vscode.window.createOutputChannel('Coyote');
  context.subscriptions.push(collection, output);

  const check = (doc) => { checkDocument(doc, collection, output); };
  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument(check),
    vscode.workspace.onDidSaveTextDocument(check),
    vscode.workspace.onDidCloseTextDocument((doc) => collection.delete(doc.uri))
  );
  vscode.workspace.textDocuments.forEach(check);

  context.subscriptions.push(
    vscode.languages.registerDocumentFormattingEditProvider('coyote', {
      async provideDocumentFormattingEdits(doc) {
        const text = doc.getText();
        if (Buffer.byteLength(text) > MAX_BYTES) return [];
        // cdt formats files, not stdin, so format a copy of the buffer through a temporary file.
        const os = require('os');
        const fs = require('fs');
        const path = require('path');
        const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'coyote-fmt-'));
        const file = path.join(dir, 'buffer.coy');
        try {
          fs.writeFileSync(file, text);
          const res = await runCdt(['fmt', file]);
          if (res.code !== 0) {
            output.appendLine(`cdt fmt failed: ${res.stderr}`);
            return [];
          }
          const full = new vscode.Range(doc.positionAt(0), doc.positionAt(text.length));
          return [vscode.TextEdit.replace(full, res.stdout)];
        } finally {
          fs.rmSync(dir, { recursive: true, force: true });
        }
      }
    })
  );
}

function deactivate() {}

module.exports = { activate, deactivate };

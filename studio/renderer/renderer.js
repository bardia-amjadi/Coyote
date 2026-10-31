'use strict';
/* global require, monaco, studio */
// Coyote Studio renderer: editor, explorer, output, problems and the debugger UI.

const $ = (id) => document.getElementById(id);
const S = {
  root: null,            // opened folder
  project: null,         // folder holding packages.json (or null)
  manifest: null,
  tabs: new Map(),       // path -> { model, dirty, viewState, name }
  active: null,
  bps: new Map(),        // path -> Set(line)
  bpDecor: new Map(),    // path -> decoration ids
  info: null,
  running: false,
  debug: { active: false, paused: false, file: null, line: 0, frame: 0, stack: [] },
  expanded: new Set(),
  selected: null,        // selected explorer path
  markers: new Map(),    // path -> diagnostics
};
let editor = null;
let debugDecor = [];

// ---------- helpers ----------
const sepOf = () => (S.info && S.info.platform === 'win32' ? '\\' : '/');
const join = (a, b) => (a.endsWith('/') || a.endsWith('\\') ? a + b : a + sepOf() + b);
const base = (p) => p.split(/[\\/]/).pop();
const dirOf = (p) => { const i = Math.max(p.lastIndexOf('/'), p.lastIndexOf('\\')); return i < 0 ? '.' : p.slice(0, i); };
const norm = (p) => p.replace(/\\/g, '/');
const esc = (s) => s.replace(/[&<>]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));
const setState = (t) => { $('st-state').textContent = t; };
const cwdFor = (file) => S.project || dirOf(file);

function resolvePath(p) {
  if (/^([A-Za-z]:[\\/]|\/)/.test(p)) return p;
  return join(S.project || S.root || '.', p);
}

// ---------- output ----------
function out(text, cls) {
  const pane = $('tab-output');
  const atEnd = pane.scrollTop + pane.clientHeight >= pane.scrollHeight - 20;
  const span = document.createElement('span');
  if (cls) span.className = cls;
  // make "file.coy:12:5" references clickable
  const re = /([^\s:"'()][^:\n"']*\.coy):(\d+)(?::(\d+))?/g;
  let last = 0, m;
  while ((m = re.exec(text))) {
    span.append(document.createTextNode(text.slice(last, m.index)));
    const a = document.createElement('a');
    a.textContent = m[0];
    const [file, line, col] = [m[1], +m[2], m[3] ? +m[3] : 1];
    a.onclick = () => openFile(resolvePath(file), line, col);
    span.append(a);
    last = m.index + m[0].length;
  }
  span.append(document.createTextNode(text.slice(last)));
  pane.append(span);
  if (atEnd) pane.scrollTop = pane.scrollHeight;
}
function outLine(text, cls) { out(text + '\n', cls); }
function showTab(name) {
  for (const b of document.querySelectorAll('#bottom-tabs button[data-tab]')) b.classList.toggle('active', b.dataset.tab === name);
  for (const id of ['output', 'problems', 'ml']) $('tab-' + id).classList.toggle('hidden', id !== name);
}

// ---------- modal ----------
function modal(html, buttons) {
  return new Promise((resolve) => {
    const box = $('modal-box');
    box.innerHTML = html + '<div class="buttons"></div>';
    const bar = box.querySelector('.buttons');
    for (const b of buttons) {
      const el = document.createElement('button');
      el.textContent = b.label;
      if (b.primary) el.className = 'primary';
      el.onclick = () => { const v = b.value && b.value(box); $('modal').classList.add('hidden'); resolve(v === undefined ? b.label : v); };
      bar.append(el);
    }
    $('modal').classList.remove('hidden');
    const first = box.querySelector('input');
    if (first) { first.focus(); first.select(); first.onkeydown = (e) => { if (e.key === 'Enter') bar.querySelector('.primary')?.click(); if (e.key === 'Escape') bar.lastChild.click(); }; }
  });
}
async function prompt(title, label, value = '') {
  const r = await modal(`<h3>${esc(title)}</h3><label>${esc(label)}</label><input id="prompt-input" type="text" value="${esc(value).replace(/"/g, '&quot;')}">`,
    [{ label: 'OK', primary: true, value: (b) => b.querySelector('#prompt-input').value.trim() }, { label: 'Cancel', value: () => null }]);
  return r || null;
}

// ---------- editor ----------
function registerLanguage() {
  monaco.languages.register({ id: 'coyote', extensions: ['.coy'], aliases: ['Coyote'] });
  monaco.languages.setLanguageConfiguration('coyote', {
    comments: { lineComment: '#' },
    brackets: [['{', '}'], ['[', ']'], ['(', ')']],
    autoClosingPairs: [{ open: '{', close: '}' }, { open: '[', close: ']' }, { open: '(', close: ')' }, { open: '"', close: '"', notIn: ['string'] }, { open: "'", close: "'", notIn: ['string', 'comment'] }],
    surroundingPairs: [{ open: '{', close: '}' }, { open: '(', close: ')' }, { open: '"', close: '"' }],
    indentationRules: { increaseIndentPattern: /\{\s*$/, decreaseIndentPattern: /^\s*\}/ },
  });
  const keywords = ['if', 'else', 'while', 'for', 'in', 'is', 'break', 'continue', 'return', 'var', 'new', 'method', 'hand', 'class', 'model', 'property', 'constructor', 'public', 'private', 'static', 'this', 'import', 'export', 'get', 'set'];
  const types = ['int', 'double', 'boolean', 'string', 'char', 'Guid', 'obj', 'dyn', 'array', 'map', 'void'];
  monaco.languages.setMonarchTokensProvider('coyote', {
    keywords, types,
    tokenizer: {
      root: [
        [/#.*$/, 'comment'],
        [/"/, 'string', '@string'],
        [/'(?:[^'\\]|\\.)'/, 'string'],
        [/\b\d+\.\d+(?:[eE][+-]?\d+)?\b/, 'number.float'],
        [/\b\d+\b/, 'number'],
        [/\b(true|false|null)\b/, 'constant'],
        [/\bhand\s*:\s*[A-Za-z_]\w*/, 'keyword'],
        [/\b(hidden|created|here|operator|module|use)\b/, 'invalid'],
        [/[A-Za-z_]\w*(?=\s*\()/, { cases: { '@keywords': 'keyword', '@default': 'entity.name.function' } }],
        [/[A-Za-z_]\w*/, { cases: { '@keywords': 'keyword', '@types': 'type', '@default': 'identifier' } }],
        [/\?\?|[=!<>]=|&&|\|\||[+\-*\/%]=?|[<>!?=.]/, 'operator'],
      ],
      string: [[/[^\\"]+/, 'string'], [/\\./, 'string.escape'], [/"/, 'string', '@pop']],
    },
  });
  monaco.editor.defineTheme('coyote-dark', {
    base: 'vs-dark', inherit: true,
    rules: [{ token: 'keyword', foreground: 'b9a3ff' }, { token: 'type', foreground: '4ec9b0' }, { token: 'entity.name.function', foreground: 'dcdcaa' }, { token: 'constant', foreground: '569cd6' }, { token: 'comment', foreground: '6a9955' }],
    colors: { 'editor.background': '#1e1e1e' },
  });
  const snippet = (label, text, doc) => ({ label, kind: monaco.languages.CompletionItemKind.Snippet, insertText: text, insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: doc });
  monaco.languages.registerCompletionItemProvider('coyote', {
    provideCompletionItems(model, pos) {
      const w = model.getWordUntilPosition(pos);
      const range = { startLineNumber: pos.lineNumber, endLineNumber: pos.lineNumber, startColumn: w.startColumn, endColumn: w.endColumn };
      const items = [
        ...keywords.map((k) => ({ label: k, kind: monaco.languages.CompletionItemKind.Keyword, insertText: k, range })),
        ...types.map((k) => ({ label: k, kind: monaco.languages.CompletionItemKind.TypeParameter, insertText: k, range })),
        { ...snippet('method', 'method ${1:name}(${2}): ${3:int} {\n\t$0\n}', 'Method'), range },
        { ...snippet('class', 'class ${1:Name} {\n\t$0\n}', 'Class'), range },
        { ...snippet('for', 'for (int ${1:i} = 0; ${1:i} < ${2:n}; ${1:i} += 1) {\n\t$0\n}', 'For loop'), range },
        { ...snippet('foreach', 'for (${1:int} ${2:x} in ${3:items}) {\n\t$0\n}', 'For-each loop'), range },
        { ...snippet('if', 'if (${1:condition}) {\n\t$0\n}', 'If'), range },
        { ...snippet('while', 'while (${1:condition}) {\n\t$0\n}', 'While'), range },
        { ...snippet('println', 'println(${1:"text"})', 'Print a line (needs hand:con println)'), range },
        { ...snippet('hand:con', 'hand:con println(string text)', 'Declare the console println host function'), range },
      ];
      const seen = new Set(items.map((i) => i.label));
      for (const m of model.getValue().matchAll(/\b[A-Za-z_]\w{2,}\b/g)) {
        if (!seen.has(m[0])) { seen.add(m[0]); items.push({ label: m[0], kind: monaco.languages.CompletionItemKind.Text, insertText: m[0], range }); }
      }
      return { suggestions: items };
    },
  });
}

function tabLabel(t) { return t.name + (t.dirty ? ' ●' : ''); }
function renderTabs() {
  const el = $('tabs');
  el.innerHTML = '';
  for (const [p, t] of S.tabs) {
    const d = document.createElement('div');
    d.className = 'tab' + (p === S.active ? ' active' : '');
    d.title = p;
    d.innerHTML = `<span>${esc(t.name)}</span><span class="dot">${t.dirty ? '●' : ''}</span><span class="x">✕</span>`;
    d.onclick = (e) => { if (e.target.classList.contains('x')) closeTab(p); else activate(p); };
    el.append(d);
  }
  $('welcome').classList.toggle('hidden', S.tabs.size > 0);
  document.title = (S.active ? base(S.active) + ' - ' : '') + 'Coyote Studio';
}

function activate(p) {
  if (S.active && S.tabs.has(S.active)) S.tabs.get(S.active).viewState = editor.saveViewState();
  S.active = p;
  const t = S.tabs.get(p);
  if (t) {
    editor.setModel(t.model);
    if (t.viewState) editor.restoreViewState(t.viewState);
    editor.focus();
  }
  renderTabs();
  refreshBreakpointDecor();
  refreshDebugLine();
  highlightTree();
}

async function openFile(p, line, col) {
  if (!S.tabs.has(p)) {
    const r = await studio.fs.read(p);
    if (!r || !r.ok) { outLine(`Cannot open ${p}: ${(r && r.error) || 'unknown error'}`, 'err'); return; }
    const model = monaco.editor.createModel(r.text, p.endsWith('.coy') ? 'coyote' : 'plaintext', monaco.Uri.file(p));
    const t = { model, dirty: false, name: base(p), saved: r.text };
    model.onDidChangeContent(() => { t.dirty = model.getValue() !== t.saved; renderTabs(); });
    S.tabs.set(p, t);
    if (p.endsWith('.coy')) runCheck(p);
  }
  activate(p);
  if (line) { editor.revealLineInCenter(line); editor.setPosition({ lineNumber: line, column: col || 1 }); }
}

async function closeTab(p) {
  const t = S.tabs.get(p);
  if (!t) return;
  if (t.dirty) {
    const r = await studio.dialog.confirm(`Save changes to ${t.name}?`, '', ['Save', "Don't save", 'Cancel']);
    if (r === 2) return;
    if (r === 0) await saveFile(p);
  }
  t.model.dispose();
  S.tabs.delete(p);
  if (S.active === p) {
    const next = [...S.tabs.keys()].pop();
    S.active = null;
    if (next) activate(next); else { editor.setModel(null); renderTabs(); }
  } else renderTabs();
}

async function saveFile(p) {
  const t = S.tabs.get(p);
  if (!t) return true;
  const text = t.model.getValue();
  const r = await studio.fs.write(p, text);
  if (r && r.ok === false) { outLine(`Cannot save ${p}: ${r.error}`, 'err'); return false; }
  t.saved = text; t.dirty = false;
  renderTabs();
  if (p.endsWith('.coy')) {
    if (S.info.settings.formatOnSave) await formatFile(p, true);
    runCheck(p);
  }
  return true;
}
async function saveAll() { for (const p of S.tabs.keys()) if (S.tabs.get(p).dirty) await saveFile(p); }

// ---------- diagnostics ----------
async function runCheck(p) {
  const r = await studio.tool.check(p, cwdFor(p));
  if (!r || !r.ok) { if (r && r.error) outLine('check: ' + r.error, 'err'); return; }
  const mine = [];
  const norm1 = (x) => norm(resolvePathFrom(x, cwdFor(p)));
  const all = r.diagnostics;
  const byFile = new Map();
  for (const d of all) {
    const f = d.file ? norm1(d.file) : norm(p);
    if (!byFile.has(f)) byFile.set(f, []);
    byFile.get(f).push(d);
  }
  S.markers.set(norm(p), byFile);
  const t = S.tabs.get(p);
  if (t) {
    const own = byFile.get(norm(p)) || [];
    monaco.editor.setModelMarkers(t.model, 'cdt', own.map((d) => {
      const lineText = t.model.getLineContent(Math.min(d.line, t.model.getLineCount()));
      const m = /^([A-Za-z_]\w*|\d+|"(?:[^"\\]|\\.)*"?|.)/.exec(lineText.slice(Math.max(0, d.column - 1)));
      return { severity: monaco.MarkerSeverity.Error, message: d.message, startLineNumber: d.line, startColumn: d.column, endLineNumber: d.line, endColumn: d.column + (m ? m[0].length : 1) };
    }));
  }
  renderProblems();
  return mine;
}
function resolvePathFrom(f, cwd) { return /^([A-Za-z]:[\\/]|\/)/.test(f) ? f : join(cwd, f); }
function renderProblems() {
  const pane = $('tab-problems');
  pane.innerHTML = '';
  let n = 0;
  for (const byFile of S.markers.values()) {
    for (const [file, ds] of byFile) {
      for (const d of ds) {
        n++;
        const a = document.createElement('a');
        a.className = 'problem';
        a.innerHTML = `<span class="err">✖</span> ${esc(d.message)}<span class="loc">${esc(base(file))}:${d.line}:${d.column}</span>`;
        a.onclick = () => openFile(file, d.line, d.column);
        pane.append(a);
      }
    }
  }
  if (!n) pane.innerHTML = '<span class="info">No problems detected in the checked files.</span>';
  $('problem-count').textContent = n ? `(${n})` : '';
}

async function formatFile(p, silent) {
  const t = S.tabs.get(p);
  if (t && t.dirty && !silent) await saveFile(p);
  const r = await studio.tool.format(p, cwdFor(p));
  if (!r.ok) { outLine('format: ' + r.message, 'err'); showTab('output'); return; }
  const fresh = await studio.fs.read(p);
  if (t && fresh.ok && fresh.text !== t.model.getValue()) { t.model.setValue(fresh.text); t.saved = fresh.text; t.dirty = false; renderTabs(); }
}

// ---------- explorer ----------
async function renderTree() {
  const el = $('tree');
  el.innerHTML = '';
  if (!S.root) return;
  async function walk(dir, depth) {
    const items = await studio.fs.list(dir);
    for (const it of items) {
      const row = document.createElement('div');
      row.className = 'node' + (S.selected === it.path ? ' active' : '');
      row.style.paddingLeft = 8 + depth * 14 + 'px';
      row.dataset.path = it.path;
      row.innerHTML = `<span class="twist">${it.dir ? (S.expanded.has(it.path) ? '▾' : '▸') : ''}</span><span class="ico">${it.dir ? '📁' : it.name.endsWith('.coy') ? '◆' : '·'}</span><span>${esc(it.name)}</span>`;
      row.onclick = async () => {
        S.selected = it.path;
        if (it.dir) { S.expanded.has(it.path) ? S.expanded.delete(it.path) : S.expanded.add(it.path); await renderTree(); } else { await openFile(it.path); }
        highlightTree();
      };
      row.oncontextmenu = (e) => { e.preventDefault(); S.selected = it.path; ctxMenu(e.clientX, e.clientY, it); };
      el.append(row);
      if (it.dir && S.expanded.has(it.path)) await walk(it.path, depth + 1);
    }
  }
  await walk(S.root, 0);
}
function highlightTree() {
  for (const n of document.querySelectorAll('#tree .node')) n.classList.toggle('active', n.dataset.path === (S.active || S.selected));
}
function ctxMenu(x, y, it) {
  const m = $('ctxmenu');
  m.innerHTML = '';
  const add = (label, fn) => { const d = document.createElement('div'); d.textContent = label; d.onclick = () => { m.classList.add('hidden'); fn(); }; m.append(d); };
  const folder = it.dir ? it.path : dirOf(it.path);
  add('New File', () => newFile(folder));
  add('New Folder', () => newFolder(folder));
  add('Rename', async () => {
    const n = await prompt('Rename', 'New name', it.name);
    if (!n) return;
    const to = join(dirOf(it.path), n);
    const r = await studio.fs.rename(it.path, to);
    if (r.ok === false) return outLine('rename: ' + r.error, 'err');
    if (S.tabs.has(it.path)) await closeTab(it.path);
    await renderTree();
  });
  add('Delete', async () => {
    if ((await studio.dialog.confirm(`Delete ${it.name}?`, 'This cannot be undone.', ['Delete', 'Cancel'])) !== 0) return;
    await studio.fs.remove(it.path);
    for (const p of [...S.tabs.keys()]) if (p === it.path || p.startsWith(it.path + sepOf())) { S.tabs.get(p).dirty = false; await closeTab(p); }
    await renderTree();
  });
  add('Reveal in File Manager', () => studio.fs.reveal(it.path));
  m.style.left = x + 'px'; m.style.top = y + 'px';
  m.classList.remove('hidden');
}
document.addEventListener('click', () => $('ctxmenu').classList.add('hidden'));

const TEMPLATE = 'hand:con println(string text)\n\nprintln("Hello from Coyote")\n';
async function newFile(dir) {
  dir = dir || S.root;
  if (!dir) return outLine('Open a folder first.', 'err');
  let n = await prompt('New file', 'File name', 'untitled.coy');
  if (!n) return;
  if (!/\.[A-Za-z0-9]+$/.test(n)) n += '.coy';
  const p = join(dir, n);
  const r = await studio.fs.create(p, n.endsWith('.coy') ? TEMPLATE : '');
  if (r.ok === false) return outLine('new file: ' + r.error, 'err');
  S.expanded.add(dir);
  await renderTree();
  await openFile(p);
}
async function newFolder(dir) {
  dir = dir || S.root;
  if (!dir) return;
  const n = await prompt('New folder', 'Folder name', 'src');
  if (!n) return;
  await studio.fs.mkdir(join(dir, n));
  S.expanded.add(dir);
  await renderTree();
}

// ---------- project ----------
async function openFolder(dir) {
  if (!dir) dir = await studio.dialog.openFolder();
  if (!dir) return;
  const r = await studio.project.open(dir);
  S.root = dir;
  S.project = r.root;
  S.manifest = r.manifest;
  S.expanded = new Set();
  $('project-name').textContent = base(dir).toUpperCase();
  await renderTree();
  const entry = r.manifest && r.manifest.ok ? join(r.root, r.manifest.entry) : null;
  if (entry) await openFile(entry).catch(() => {});
  setState(r.root ? 'Project: ' + base(r.root) : 'Folder opened (no packages.json)');
  refreshRecent();
}
async function newProject() {
  const parent = await studio.dialog.openFolder();
  if (!parent) return;
  const name = await prompt('New project', 'Project name (letters, digits, _ - .)', 'my-app');
  if (!name) return;
  const kind = ((await prompt('New project', 'Type: app (a program) or library (a .lib others can `use`)', 'app')) || 'app').trim().toLowerCase();
  if (kind !== 'app' && kind !== 'library') return outLine('new project: type must be "app" or "library"', 'err');
  const r = await studio.project.create(parent, name, kind);
  if (!r.ok) return outLine('new project: ' + r.error, 'err');
  await openFolder(r.dir);
}
async function refreshRecent() {
  const info = await studio.info();
  S.info = info;
  const ul = $('recent');
  ul.innerHTML = '';
  for (const d of info.settings.recent || []) {
    const li = document.createElement('li');
    li.textContent = d;
    li.onclick = () => openFolder(d);
    ul.append(li);
  }
}

// ---------- run / build ----------
function setRunning(v) {
  S.running = v;
  $('btn-stop').disabled = !v;
  for (const id of ['btn-run', 'btn-project', 'btn-debug', 'btn-native', 'btn-cpm']) $(id).disabled = v;
  setState(v ? 'Running...' : 'Ready');
}
function activeCt() { return S.active && S.active.endsWith('.coy') ? S.active : null; }

async function runActive() {
  const f = activeCt();
  if (!f) return outLine('Open a .coy file to run it.', 'err');
  await saveAll();
  showTab('output');
  outLine(`> run ${base(f)}`, 'info');
  setRunning(true);
  const r = await studio.run.file(f, cwdFor(f));
  if (r.ok === false) { outLine(r.error, 'err'); setRunning(false); }
}
async function runProject() {
  if (!S.project) return outLine('This folder has no packages.json. Use New Project, or run the current file.', 'err');
  if (S.manifest && S.manifest.manifest && S.manifest.manifest.type === 'library') return outLine('This is a library project: it cannot run. Use Tools > Build Library, then `use` it from an app.', 'err');
  await saveAll();
  showTab('output');
  outLine(`> cpx start (${base(S.project)})`, 'info');
  setRunning(true);
  const r = await studio.run.workflow(S.project, 'start');
  if (r.ok === false) { outLine(r.error, 'err'); setRunning(false); }
}
async function packLib(install) {
  if (!S.project) return outLine('This folder has no packages.json. Create a library with New Project (type: library).', 'err');
  await saveAll();
  showTab('output');
  outLine(`> coyote lib pack ${base(S.project)}${install ? ' (and install for the current user)' : ''}`, 'info');
  const r = await studio.lib.pack(S.project, install);
  if (!r.ok) return outLine(r.output || 'library build failed', 'err');
  outLine(r.output, 'good');
  if (r.installedTo) outLine(`Installed to ${r.installedTo}. Any program can now write: use "${base(r.file).replace(/\.lib$/, '')}"`, 'good');
  else outLine(`Library written: ${r.file}. Use it with: use "./path/to/${base(r.file)}"`, 'good');
}
async function cpmInstall() {
  if (!S.project) return outLine('This folder has no packages.json.', 'err');
  showTab('output');
  outLine('> cpm install', 'info');
  setRunning(true);
  const r = await studio.run.cpm(S.project, ['install']);
  if (r.ok === false) { outLine(r.error, 'err'); setRunning(false); }
}
async function showMl() {
  const f = activeCt();
  if (!f) return;
  await saveAll();
  const r = await studio.tool.showMl(f, cwdFor(f));
  $('ml-text').textContent = r.text;
  showTab('ml');
}
async function buildNative() {
  const f = activeCt();
  if (!f) return;
  await saveAll();
  const def = f.replace(/\.coy$/, S.info.platform === 'win32' ? '.exe' : '');
  const out_ = await studio.dialog.saveExe(def);
  if (!out_) return;
  showTab('output');
  outLine(`> native build ${base(f)} -> ${out_}`, 'info');
  setState('Building native executable...');
  const r = await studio.tool.native(f, out_, cwdFor(f));
  if (r.ok) outLine('Native executable written: ' + out_, 'good');
  else outLine((r.phase === 'native' ? 'Native backend: ' : 'Compile error:\n') + r.output + (r.phase === 'native' ? '\n(the native backend supports only a subset; run on the PVM instead)' : ''), 'err');
  setState('Ready');
}

// ---------- debugger ----------
function bpSet(file) { if (!S.bps.has(file)) S.bps.set(file, new Set()); return S.bps.get(file); }
function refreshBreakpointDecor() {
  if (!S.active) return;
  const t = S.tabs.get(S.active);
  if (!t) return;
  const lines = [...(S.bps.get(S.active) || [])];
  const ids = t.model.deltaDecorations(S.bpDecor.get(S.active) || [], lines.map((l) => ({ range: new monaco.Range(l, 1, l, 1), options: { glyphMarginClassName: 'bp-glyph', stickiness: monaco.editor.TrackedRangeStickiness.NeverGrowsWhenTypingAtEdges } })));
  S.bpDecor.set(S.active, ids);
  renderBps();
}
function renderBps() {
  const el = $('bps');
  el.innerHTML = '';
  let n = 0;
  for (const [f, set] of S.bps) for (const l of [...set].sort((a, b) => a - b)) {
    n++;
    const d = document.createElement('div');
    d.className = 'row';
    d.innerHTML = `<span style="color:#e51400">●</span> ${esc(base(f))}:${l}`;
    d.style.cursor = 'pointer';
    d.onclick = () => openFile(f, l);
    el.append(d);
  }
  if (!n) el.innerHTML = '<div class="empty">none (click the left margin of the editor)</div>';
}
async function toggleBreakpoint(line) {
  const f = S.active;
  if (!f || !f.endsWith('.coy')) return;
  line = line || editor.getPosition().lineNumber;
  const set = bpSet(f);
  if (set.has(line)) { set.delete(line); if (S.debug.active) studio.debug.unbreak(f, line); } else { set.add(line); if (S.debug.active) studio.debug.breakpoint(f, line); }
  refreshBreakpointDecor();
}
function refreshDebugLine() {
  const t = S.active && S.tabs.get(S.active);
  if (!t) return;
  const here = S.debug.active && S.debug.paused && S.debug.file && norm(S.debug.file) === norm(S.active);
  debugDecor = t.model.deltaDecorations(debugDecor, here ? [{ range: new monaco.Range(S.debug.line, 1, S.debug.line, 1), options: { isWholeLine: true, className: 'debug-line', glyphMarginClassName: 'debug-glyph' } }] : []);
}

function row(html, cls) { const d = document.createElement('div'); d.className = 'row ' + (cls || ''); d.innerHTML = html; return d; }
async function loadFrame(i) {
  S.debug.frame = i;
  const vars = await studio.debug.vars(i);
  const el = $('vars');
  el.innerHTML = '';
  const list = (vars && vars.result) || [];
  for (const v of list) el.append(row(`<span class="name">${esc(v.name)}</span> <span class="type">${esc(v.type)}</span> = <span class="val">${esc(v.value)}</span>`));
  if (!list.length) el.innerHTML = '<div class="empty">no variables</div>';
  for (const [k, n] of S.debug.stack.entries()) n.el && n.el.classList.toggle('sel', k === i);
  const fr = S.debug.stack[i];
  if (fr && fr.line) { S.debug.file = resolvePath(fr.file); S.debug.line = fr.line; await openFile(S.debug.file, fr.line); refreshDebugLine(); }
}
async function onStopped(ev) {
  S.debug.paused = true;
  S.debug.file = resolvePath(ev.file);
  S.debug.line = ev.line;
  setState(`Paused at ${base(ev.file)}:${ev.line} (${ev.reason})`);
  const st = await studio.debug.stack();
  const frames = (st && st.result) || [];
  S.debug.stack = frames;
  const el = $('stack');
  el.innerHTML = '';
  frames.forEach((f, i) => { const r = row(`${esc(f.function)} <span class="type">${esc(base(f.file))}:${f.line}</span>`, i === 0 ? 'sel' : ''); r.style.cursor = 'pointer'; r.onclick = () => loadFrame(i); f.el = r; el.append(r); });
  await openFile(S.debug.file, ev.line);
  await loadFrame(0);
  refreshDebugLine();
}
function debugUi(on) {
  S.debug.active = on;
  $('debugbar').classList.toggle('hidden', !on);
  if (!on) { S.debug.paused = false; refreshDebugLine(); $('vars').innerHTML = ''; $('stack').innerHTML = ''; }
}
function resumeUi() { S.debug.paused = false; refreshDebugLine(); setState('Running (debug)...'); }

async function startDebug() {
  const f = activeCt() || (S.project && S.manifest && S.manifest.ok ? join(S.project, S.manifest.entry) : null);
  if (!f) return outLine('Open a .coy file to debug it.', 'err');
  await saveAll();
  showTab('output');
  outLine(`> debug ${base(f)}`, 'info');
  const bps = [];
  for (const [file, set] of S.bps) for (const line of set) bps.push({ file, line });
  setRunning(true);
  const r = await studio.debug.start(f, cwdFor(f), bps);
  if (!r.ok) { outLine(r.error, 'err'); setRunning(false); return; }
  debugUi(true);
}
async function debugCmd(name) { resumeUi(); await studio.debug[name](); }

// ---------- dialogs ----------
async function showSettings() {
  const info = await studio.info();
  const s = info.settings;
  const r = await modal(`<h3>Settings</h3>
    <label>Toolchain folder (leave empty to use the installed Coyote)</label>
    <input id="set-tc" type="text" value="${esc(s.toolchainDir || '').replace(/"/g, '&quot;')}">
    <p class="info" style="color:var(--muted)">Using: ${esc(info.toolchain.dir || 'not found')}</p>
    <label>Editor font size</label><input id="set-font" type="text" value="${s.fontSize}">
    <label><input id="set-fos" type="checkbox" ${s.formatOnSave ? 'checked' : ''}> Format on save</label>`,
    [{ label: 'Save', primary: true, value: (b) => ({ tc: b.querySelector('#set-tc').value.trim(), font: parseInt(b.querySelector('#set-font').value, 10) || 14, fos: b.querySelector('#set-fos').checked }) }, { label: 'Cancel', value: () => null }]);
  if (!r) return;
  await studio.setSetting('formatOnSave', r.fos);
  await studio.setSetting('fontSize', r.font);
  const x = await studio.setSetting('toolchainDir', r.tc);
  editor.updateOptions({ fontSize: r.font });
  S.info = await studio.info();
  updateToolchainStatus();
  if (!x.toolchain.dir) outLine('The Coyote tools were not found in that folder.', 'err');
}
async function showAbout() {
  const v = await studio.versions();
  const info = await studio.info();
  const lines = Object.entries(v).map(([k, val]) => `${k.padEnd(12)} ${val || 'not installed'}`).join('\n');
  await modal(`<h3>Coyote Studio ${esc(info.version)}</h3><pre>${esc(lines)}</pre><p style="color:var(--muted)">Toolchain: ${esc(info.toolchain.dir || 'not found')}</p>`, [{ label: 'Close', primary: true }]);
}
async function showDoctor() {
  const r = await studio.doctor();
  await modal(`<h3>Toolchain doctor</h3><pre>${esc(r.text || r.error || '')}</pre>`, [{ label: 'Close', primary: true }]);
}
function updateToolchainStatus() {
  const tc = S.info.toolchain;
  $('st-tool').textContent = tc.dir ? 'Coyote toolchain: ' + tc.dir : 'Coyote toolchain NOT FOUND';
  const w = $('toolchain-warning');
  if (!tc.dir) { w.textContent = 'The Coyote tools (coyote, pvm) were not found. Install Coyote with the setup program, or set the toolchain folder in Settings.'; w.classList.remove('hidden'); } else if (tc.missing.length) { w.textContent = 'Some tools are missing: ' + tc.missing.join(', '); w.classList.remove('hidden'); } else w.classList.add('hidden');
}

// ---------- menu + wiring ----------
const MENU = {
  newProject, newFile: () => newFile(), openFolder: () => openFolder(), openFile: async () => { const f = await studio.dialog.openFile(); if (f) openFile(f); },
  save: () => S.active && saveFile(S.active), saveAll, format: () => activeCt() && formatFile(activeCt()),
  run: runActive, runProject, debug: startDebug, stop: () => studio.run.stop(),
  continue: () => S.debug.active && debugCmd('resume'), stepOver: () => S.debug.active && debugCmd('stepOver'), stepInto: () => S.debug.active && debugCmd('stepInto'), stepOut: () => S.debug.active && debugCmd('stepOut'),
  toggleBreakpoint: () => toggleBreakpoint(), check: async () => { const f = activeCt(); if (f) { await saveAll(); await runCheck(f); showTab('problems'); } },
  showMl, native: buildNative, packLib: () => packLib(false), installLib: () => packLib(true), cpmInstall, doctor: showDoctor, settings: showSettings, about: showAbout,
};

async function init() {
  S.info = await studio.info();
  $('welcome-version').textContent = 'Version ' + S.info.version;
  updateToolchainStatus();
  await refreshRecent();

  await new Promise((resolve) => {
    const baseUrl = new URL('../node_modules/monaco-editor/min', location.href).href;
    require.config({ paths: { vs: baseUrl + '/vs' } });
    self.MonacoEnvironment = { getWorkerUrl: () => 'data:text/javascript;charset=utf-8,' + encodeURIComponent(`self.MonacoEnvironment={baseUrl:'${baseUrl}/'};importScripts('${baseUrl}/vs/base/worker/workerMain.js');`) };
    require(['vs/editor/editor.main'], resolve);
  });
  registerLanguage();
  editor = monaco.editor.create($('editor'), { theme: 'coyote-dark', automaticLayout: true, glyphMargin: true, fontSize: S.info.settings.fontSize || 14, minimap: { enabled: false }, scrollBeyondLastLine: false, model: null, fontFamily: 'Consolas, "Cascadia Mono", monospace', renderWhitespace: 'selection' });
  editor.onDidChangeCursorPosition((e) => { $('st-pos').textContent = `Ln ${e.position.lineNumber}, Col ${e.position.column}`; });
  editor.onMouseDown((e) => { if (e.target.type === monaco.editor.MouseTargetType.GUTTER_GLYPH_MARGIN) toggleBreakpoint(e.target.position.lineNumber); });
  window.__studio = { S, monaco, editor, openFile, MENU, openFolderPath: openFolder };  // used by the automated UI test

  $('btn-run').onclick = runActive; $('btn-project').onclick = runProject; $('btn-debug').onclick = startDebug;
  $('btn-stop').onclick = () => studio.run.stop();
  $('btn-check').onclick = MENU.check; $('btn-format').onclick = MENU.format; $('btn-ml').onclick = showMl; $('btn-native').onclick = buildNative; $('btn-cpm').onclick = cpmInstall;
  $('btn-settings').onclick = showSettings;
  $('dbg-continue').onclick = MENU.continue; $('dbg-over').onclick = MENU.stepOver; $('dbg-into').onclick = MENU.stepInto; $('dbg-out').onclick = MENU.stepOut;
  $('ex-newfile').onclick = () => newFile(); $('ex-newdir').onclick = () => newFolder(); $('ex-refresh').onclick = renderTree;
  $('w-open').onclick = () => openFolder(); $('w-new').onclick = newProject; $('w-file').onclick = MENU.openFile;
  $('clear-output').onclick = () => { $('tab-output').innerHTML = ''; };
  for (const b of document.querySelectorAll('#bottom-tabs button[data-tab]')) b.onclick = () => showTab(b.dataset.tab);

  studio.onMenu((a) => { if (MENU[a]) MENU[a](); });
  studio.onOpenPath((f) => openFile(f));
  studio.takeOpenPath().then((f) => { if (f) openFile(f); });
  studio.onRunOut(({ phase, text }) => out(text));
  studio.onRunErr(({ phase, text }) => out(text, 'err'));
  studio.onRunExit((r) => {
    outLine(r.code === 0 ? '[finished]' : `[exited with code ${r.code}${r.error ? ': ' + r.error : ''}]`, r.code === 0 ? 'good' : 'err');
    setRunning(false);
  });
  studio.onDebug(async (ev) => {
    if (ev.event === 'output') out(ev.text);
    else if (ev.event === 'log') out(ev.text, 'err');
    else if (ev.event === 'stopped') await onStopped(ev);
    else if (ev.event === 'runtimeError') { outLine(`${ev.file}:${ev.line}: runtime error: ${ev.message}`, 'err'); await openFile(resolvePath(ev.file), ev.line); }
    else if (ev.event === 'exited') { outLine(ev.code === 0 ? '[finished]' : `[exited with code ${ev.code}]`, ev.code === 0 ? 'good' : 'err'); debugUi(false); setRunning(false); }
  });
  renderBps();
  renderProblems();
}

window.addEventListener('DOMContentLoaded', () => { init().catch((e) => { document.body.insertAdjacentHTML('beforeend', `<pre style="color:#f88;padding:20px">Coyote Studio failed to start: ${esc(String(e && e.stack || e))}</pre>`); }); });

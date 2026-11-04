'use strict';
/* global monaco, studio, CoyoteLang, require */
// Editor side of the Studio renderer: Monaco setup, tabs, library/workspace indexes, diagnostics, saving.
const S = App.S;
const L = CoyoteLang;
let editor = null;
let debugDecor = [];
const checkTimers = new Map();

// ---------- indexes (libraries and workspace) ----------
async function loadLibs() {
  const r = await studio.lang.libs(S.project || S.root || '');
  const parsed = [];
  if (r && r.libs) for (const f of r.libs) parsed.push({ lib: L.parseLib(f.text), path: f.file });
  S.libs = L.buildLibIndex(parsed);
  S.libDirs = (r && r.dirs) || [];
  S.gen++;
  if (App.renderLibsView) App.renderLibsView();
  $('st-libs').textContent = `${S.libs.size} libraries`;
  refreshAllChecks();
}

function indexFile(path, text) {
  const ix = L.indexSource(text);
  const tag = (s) => { s.path = path; return s; };
  for (const c of ix.classes) { tag(c); c.members.forEach(tag); }
  ix.methods.forEach(tag);
  S.ws.set(keyOf(path), { path, ix, text });
  S.gen++;
}
async function loadWorkspace() {
  S.ws = new Map();
  if (!S.root) return;
  const r = await studio.search.files(S.root);
  const files = ((r && r.files) || []).filter((f) => f.endsWith('.coy')).slice(0, 400);
  for (const f of files) {
    const open = S.tabs.get(f);
    if (open) { indexFile(f, open.model.getValue()); continue; }
    const t = await studio.fs.read(f);
    if (t && t.ok && t.text.length < 400000) indexFile(f, t.text);
  }
  S.gen++;
}

// Language service host (see editor-lang.js)
function fileOfModel(model) {
  const key = S.byUri.get(model.uri.toString());
  const t = key && S.tabs.get(key);
  return t && t.path ? t.path : '';
}
const usedNamespaces = (text) => new Set([...text.matchAll(/^\s*use\s+"([^"]+)"/gm)].map((m) => m[1]));
const ownNamespace = (text) => { const m = /^\s*namespace\s+([A-Za-z_][\w.]*)/m.exec(text); return m ? m[1] : ''; };
// Default namespace for a file: <project name>.<folders below the project, without src>, each part capitalised.
function suggestNamespace(path) {
  const cap = (s) => s.replace(/[^A-Za-z0-9_]/g, '').replace(/^./, (c) => c.toUpperCase());
  const root = norm(S.project || S.root || '');
  const parts = [cap((S.manifest && S.manifest.name) || base(root) || 'App')];
  const dir = norm(dirOf(norm(path || '')));
  if (root && dir.toLowerCase().startsWith(root.toLowerCase()) && dir.length > root.length) {
    for (const seg of dir.slice(root.length + 1).split('/')) if (seg && seg.toLowerCase() !== 'src' && cap(seg)) parts.push(cap(seg));
  }
  return parts.filter(Boolean).join('.');
}
const langHost = {
  libs: () => S.libs,
  settings: () => S.info.settings,
  generation: () => S.gen,
  // Exported symbols of the namespaces this file uses, plus its own namespace (files of one namespace see each other).
  importedSymbols(model) {
    const out = { classes: [], methods: [] };
    const text = model.getValue();
    const from = fileOfModel(model);
    const names = usedNamespaces(text);
    const own = ownNamespace(text);
    if (own) names.add(own);
    if (!names.size) return out;
    for (const e of S.ws.values()) {
      if (!e.ix.namespace || !names.has(e.ix.namespace) || (from && keyOf(e.path) === keyOf(from))) continue;
      for (const c of e.ix.classes) if (c.exported) out.classes.push(c);
      for (const x of e.ix.methods) if (x.exported && !x.extension) out.methods.push(x);
    }
    return out;
  },
  // Exported symbols of namespaces not used yet: choosing one adds `use "<namespace>"`.
  importCandidates(model) {
    const text = model.getValue();
    const from = fileOfModel(model);
    const have = usedNamespaces(text);
    const own = ownNamespace(text);
    if (own) have.add(own);
    const out = [];
    for (const e of S.ws.values()) {
      const spec = e.ix.namespace;
      if (!spec || have.has(spec) || (from && keyOf(e.path) === keyOf(from))) continue;
      for (const c of e.ix.classes) if (c.exported) out.push({ sym: c, spec });
      for (const m of e.ix.methods) if (m.exported && !m.extension) out.push({ sym: m, spec });
    }
    return out;
  },
  // Namespaces of this project for `use "..."` completion.
  namespaces(model) {
    const own = ownNamespace(model.getValue());
    const map = new Map();
    for (const e of S.ws.values()) {
      const ns = e.ix.namespace;
      if (!ns || ns === own) continue;
      const item = map.get(ns) || { ns, files: 0, exports: [] };
      item.files++;
      for (const c of e.ix.classes) if (c.exported) item.exports.push(c.name);
      for (const m of e.ix.methods) if (m.exported && !m.extension) item.exports.push(m.name);
      map.set(ns, item);
    }
    return [...map.values()].sort((a, b) => a.ns.localeCompare(b.ns));
  },
  // Namespaces for the `namespace ...` line: the ones already in the project plus the one that matches this file's folder.
  namespaceSuggestions(model) {
    const names = new Set([suggestNamespace(fileOfModel(model))]);
    for (const e of S.ws.values()) if (e.ix.namespace) names.add(e.ix.namespace);
    return [...names];
  },
  ensureLibModel(libName, file) {
    const uri = monaco.Uri.parse(`coyote-lib://${encodeURIComponent(libName)}/${file}`);
    if (!monaco.editor.getModel(uri)) {
      const lib = S.libs.get(libName.toLowerCase());
      const src = lib && lib.sources[file];
      if (src === undefined) return null;
      monaco.editor.createModel(src, 'coyote', uri);
    }
    return uri;
  },
  ensureFileModel(path) {
    const uri = monaco.Uri.file(path);
    if (!monaco.editor.getModel(uri)) {
      const e = S.ws.get(keyOf(path));
      if (!e) return null;
      monaco.editor.createModel(e.text, 'coyote', uri);
    }
    return uri;
  },
  async formatText(text) { return studio.tool.formatText(text); },
};

// ---------- tabs ----------
function renderTabs() {
  const el = $('tabs');
  el.innerHTML = '';
  for (const [k, t] of S.tabs) {
    const d = document.createElement('div');
    d.className = 'tab' + (k === S.active ? ' active' : '') + (t.readOnly ? ' ro' : '');
    d.title = t.readOnly ? `${t.name} (read-only, library ${t.lib})` : k;
    d.innerHTML = `<span>${esc(t.name)}</span><span class="dot">${t.dirty ? '●' : ''}</span><span class="x">✕</span>`;
    d.onclick = (e) => { if (e.target.classList.contains('x')) closeTab(k); else activate(k); };
    d.onauxclick = (e) => { if (e.button === 1) closeTab(k); };
    d.oncontextmenu = (e) => { e.preventDefault(); App.ctxMenu(e.clientX, e.clientY, [
      { label: 'Close', run: () => closeTab(k) }, { label: 'Close Others', run: () => closeOthers(k) }, { label: 'Close All', run: closeAll },
      ...(t.path && !t.readOnly ? ['-', { label: 'Reveal in File Manager', run: () => studio.fs.reveal(t.path) }] : [])]); };
    el.append(d);
  }
  $('welcome').classList.toggle('hidden', S.tabs.size > 0);
  const t = S.active && S.tabs.get(S.active);
  document.title = (t ? t.name + ' - ' : '') + 'Coyote Studio';
  renderBreadcrumbs();
}

function activate(k) {
  if (S.active && S.tabs.has(S.active)) S.tabs.get(S.active).viewState = editor.saveViewState();
  S.active = k;
  const t = S.tabs.get(k);
  if (t) {
    editor.setModel(t.model);
    editor.updateOptions({ readOnly: !!t.readOnly });
    if (t.viewState) editor.restoreViewState(t.viewState);
    editor.focus();
  }
  renderTabs();
  refreshBreakpointDecor();
  refreshDebugLine();
  if (App.highlightTree) App.highlightTree();
}

function watchModel(t) {
  t.model.onDidChangeContent(() => {
    t.dirty = t.model.getValue() !== t.saved;
    renderTabs();
    if (t.path && !t.readOnly && t.path.endsWith('.coy')) {
      scheduleCheck(t.key);
      clearTimeout(t.indexTimer);
      t.indexTimer = setTimeout(() => indexFile(t.path, t.model.getValue()), 400);
      if (S.info.settings.autoSave === 'afterDelay') { clearTimeout(t.saveTimer); t.saveTimer = setTimeout(() => saveFile(t.key), 1200); }
    }
  });
}

async function openFile(p, line, col) {
  if (!S.tabs.has(p)) {
    const r = await studio.fs.read(p);
    if (!r || !r.ok) { App.out(`Cannot open ${p}: ${(r && r.error) || 'unknown error'}\n`, 'err'); return; }
    const uri = monaco.Uri.file(p);
    let model = monaco.editor.getModel(uri);
    if (model) model.setValue(r.text); else model = monaco.editor.createModel(r.text, /\.coy$/i.test(p) ? 'coyote' : /\.json$/i.test(p) ? 'json' : /\.md$/i.test(p) ? 'markdown' : 'plaintext', uri);
    const t = { key: p, path: p, model, dirty: false, name: base(p), saved: r.text, readOnly: false };
    S.tabs.set(p, t);
    S.byUri.set(uri.toString(), p);
    watchModel(t);
    if (p.endsWith('.coy')) { indexFile(p, r.text); scheduleCheck(p, 0); }
  }
  activate(p);
  if (line) { editor.revealLineInCenter(line); editor.setPosition({ lineNumber: line, column: col || 1 }); }
}

// A library source shown read-only (go to definition on a library symbol, Libraries view).
function openVirtual(uri, line) {
  const key = uri.toString();
  if (!S.tabs.has(key)) {
    const model = monaco.editor.getModel(uri);
    if (!model) return;
    const lib = decodeURIComponent(uri.authority);
    const file = uri.path.replace(/^\//, '');
    S.tabs.set(key, { key, path: '', model, dirty: false, saved: model.getValue(), name: `${file} (${lib})`, readOnly: true, lib });
    S.byUri.set(key, key);
  }
  activate(key);
  if (line) { editor.revealLineInCenter(line); editor.setPosition({ lineNumber: line, column: 1 }); }
}
function openLibSource(libName, file, line) {
  const uri = langHost.ensureLibModel(libName, file);
  if (uri) openVirtual(uri, line);
}

async function closeTab(k) {
  const t = S.tabs.get(k);
  if (!t) return true;
  if (t.dirty) {
    const r = await studio.dialog.confirm(`Save changes to ${t.name}?`, '', ['Save', "Don't save", 'Cancel']);
    if (r === 2) return false;
    if (r === 0) await saveFile(k);
  }
  if (t.readOnly) { /* keep the virtual model for peek views */ } else t.model.dispose();
  S.tabs.delete(k);
  S.byUri.delete(t.model.uri.toString());
  if (S.active === k) {
    const next = [...S.tabs.keys()].pop();
    S.active = null;
    if (next) activate(next); else { editor.setModel(null); renderTabs(); }
  } else renderTabs();
  return true;
}
async function closeOthers(k) { for (const x of [...S.tabs.keys()]) if (x !== k) await closeTab(x); }
async function closeAll() { for (const x of [...S.tabs.keys()]) await closeTab(x); }

async function saveFile(k) {
  const t = S.tabs.get(k);
  if (!t || t.readOnly) return true;
  const text = t.model.getValue();
  const r = await studio.fs.write(t.path, text);
  if (r && r.ok === false) { App.toast(`Cannot save ${t.name}: ${r.error}`, 'error'); return false; }
  t.saved = text; t.dirty = false;
  renderTabs();
  if (t.path.endsWith('.coy')) {
    if (S.info.settings.formatOnSave && k === S.active) { await editor.getAction('editor.action.formatDocument').run(); if (t.model.getValue() !== text) { const again = t.model.getValue(); await studio.fs.write(t.path, again); t.saved = again; t.dirty = false; renderTabs(); } }
    indexFile(t.path, t.model.getValue());
    scheduleCheck(k, 0);
  }
  return true;
}
async function saveAll() { for (const k of S.tabs.keys()) if (S.tabs.get(k).dirty) await saveFile(k); }

// ---------- diagnostics ----------
function scheduleCheck(k, delay) {
  if (S.info && S.info.settings.checkOnType === false && delay !== 0) return;
  clearTimeout(checkTimers.get(k));
  checkTimers.set(k, setTimeout(() => runCheck(k), delay === undefined ? 500 : delay));
}
function refreshAllChecks() { for (const [k, t] of S.tabs) if (t.path && t.path.endsWith('.coy')) scheduleCheck(k, 0); }

async function runCheck(k) {
  const t = S.tabs.get(k);
  if (!t || !t.path) return;
  const version = t.model.getVersionId();
  const text = t.model.getValue();
  const r = await studio.tool.checkText(t.path, text, cwdFor(t.path));
  if (!r || !r.ok) return;
  if (!S.tabs.has(k) || t.model.getVersionId() !== version) return;  // typed meanwhile; a newer check is queued
  const byFile = new Map();
  for (const d of r.diagnostics) {
    const f = d.file ? norm(resolvePath(d.file, cwdFor(t.path))) : norm(t.path);
    if (!byFile.has(f)) byFile.set(f, []);
    byFile.get(f).push(d);
  }
  S.markers.set(norm(t.path), byFile);
  const own = byFile.get(norm(t.path)) || [];
  monaco.editor.setModelMarkers(t.model, 'cdt', own.map((d) => {
    const ln = Math.min(Math.max(d.line, 1), t.model.getLineCount());
    const lineText = t.model.getLineContent(ln);
    const m = /^([A-Za-z_]\w*|\d+(?:\.\d+)?|"(?:[^"\\]|\\.)*"?|.)/.exec(lineText.slice(Math.max(0, d.column - 1)));
    return { severity: d.severity === 'warning' ? monaco.MarkerSeverity.Warning : monaco.MarkerSeverity.Error, message: d.message, startLineNumber: ln, startColumn: d.column, endLineNumber: ln, endColumn: d.column + (m ? m[0].length : 1) };
  }));
  renderProblems();
}

function problemList() {
  const all = [];
  const seen = new Set();
  for (const byFile of S.markers.values()) for (const [file, ds] of byFile) for (const d of ds) {
    const id = `${file}:${d.line}:${d.column}:${d.message}`;
    if (!seen.has(id)) { seen.add(id); all.push({ file, ...d }); }
  }
  return all;
}
function renderProblems() {
  const pane = $('pane-problems');
  pane.innerHTML = '';
  const list = problemList();
  for (const d of list) {
    const a = document.createElement('a');
    a.className = 'problem';
    a.innerHTML = `<span class="err">✖</span> ${esc(d.message)}<span class="loc">${esc(base(d.file))} [${d.line}, ${d.column}]</span>`;
    a.onclick = () => openFile(resolvePath(d.file), d.line, d.column);
    pane.append(a);
  }
  if (!list.length) pane.innerHTML = '<span class="info">No problems detected.</span>';
  $('problem-count').textContent = list.length ? String(list.length) : '';
  $('st-problems').textContent = `✖ ${list.length}`;
}

// ---------- breakpoints and debug line ----------
function bpSet(file) { if (!S.bps.has(file)) S.bps.set(file, new Set()); return S.bps.get(file); }
function refreshBreakpointDecor() {
  const t = S.active && S.tabs.get(S.active);
  if (t && t.path) {
    const lines = [...(S.bps.get(t.path) || [])];
    const ids = t.model.deltaDecorations(S.bpDecor.get(t.path) || [], lines.map((l) => ({ range: new monaco.Range(l, 1, l, 1), options: { glyphMarginClassName: 'bp-glyph', stickiness: monaco.editor.TrackedRangeStickiness.NeverGrowsWhenTypingAtEdges } })));
    S.bpDecor.set(t.path, ids);
  }
  if (App.renderBps) App.renderBps();
}
function toggleBreakpoint(line) {
  const t = S.active && S.tabs.get(S.active);
  if (!t || !t.path || !t.path.endsWith('.coy')) return;
  const f = t.path;
  line = line || editor.getPosition().lineNumber;
  const set = bpSet(f);
  if (set.has(line)) { set.delete(line); if (S.debug.active) studio.debug.unbreak(f, line); } else { set.add(line); if (S.debug.active) studio.debug.breakpoint(f, line); }
  refreshBreakpointDecor();
}
function refreshDebugLine() {
  const t = S.active && S.tabs.get(S.active);
  if (!t) return;
  const here = S.debug.active && S.debug.paused && S.debug.file && t.path && norm(S.debug.file) === norm(t.path);
  debugDecor = t.model.deltaDecorations(debugDecor, here ? [{ range: new monaco.Range(S.debug.line, 1, S.debug.line, 1), options: { isWholeLine: true, className: 'debug-line', glyphMarginClassName: 'debug-glyph' } }] : []);
}

// ---------- breadcrumbs ----------
function renderBreadcrumbs() {
  const el = $('breadcrumbs');
  const t = S.active && S.tabs.get(S.active);
  if (!t) { el.innerHTML = ''; return; }
  const parts = [];
  if (t.readOnly) parts.push(`library ${t.lib}`, t.name.replace(/ \(.*\)$/, ''));
  else {
    const rel = S.root && norm(t.path).toLowerCase().startsWith(norm(S.root).toLowerCase()) ? norm(t.path).slice(norm(S.root).length + 1) : t.path;
    parts.push(...norm(rel).split('/'));
  }
  const pos = editor && editor.getPosition();
  if (pos && /\.coy$/i.test(t.name.replace(/ \(.*\)$/, ''))) {
    const ix = L.indexSource(t.model.getValue());
    const enc = L.enclosing(ix, pos.lineNumber);
    if (enc.cls) parts.push(enc.cls.name);
    if (enc.method) parts.push(enc.method.kind === 'ctor' ? 'constructor' : enc.method.name);
  }
  el.innerHTML = parts.map((p, i) => `<span class="crumb">${esc(p)}</span>${i < parts.length - 1 ? '<span>›</span>' : ''}`).join('');
  for (const c of el.querySelectorAll('.crumb')) c.onclick = () => editor.getAction('editor.action.quickOutline').run();
}

// ---------- Monaco setup ----------
async function initMonaco() {
  await new Promise((resolve) => {
    const baseUrl = new URL('../node_modules/monaco-editor/min', location.href).href;
    require.config({ paths: { vs: baseUrl + '/vs' } });
    self.MonacoEnvironment = { getWorkerUrl: () => 'data:text/javascript;charset=utf-8,' + encodeURIComponent(`self.MonacoEnvironment={baseUrl:'${baseUrl}/'};importScripts('${baseUrl}/vs/base/worker/workerMain.js');`) };
    require(['vs/editor/editor.main'], resolve);
  });
  window.registerCoyoteLanguage(monaco, langHost);
  const st = S.info.settings;
  editor = monaco.editor.create($('editor'), {
    theme: st.theme === 'light' ? 'coyote-light' : 'coyote-dark', automaticLayout: true, glyphMargin: true, model: null,
    fontSize: st.fontSize || 14, fontFamily: st.fontFamily || 'Cascadia Mono, Consolas, monospace', fontLigatures: false,
    minimap: { enabled: !!st.minimap }, wordWrap: st.wordWrap ? 'on' : 'off', tabSize: st.tabSize || 4, insertSpaces: true, detectIndentation: false,
    scrollBeyondLastLine: false, renderWhitespace: 'selection', smoothScrolling: true, cursorSmoothCaretAnimation: 'on', bracketPairColorization: { enabled: true },
    guides: { bracketPairs: true, indentation: true }, 'semanticHighlighting.enabled': true, quickSuggestions: { other: true, comments: false, strings: true },
    suggest: { showWords: false, preview: true, showIcons: true }, parameterHints: { enabled: true }, stickyScroll: { enabled: true }, lineNumbersMinChars: 3,
    padding: { top: 6 }, fixedOverflowWidgets: true, acceptSuggestionOnCommitCharacter: true,
  });
  App.editor = editor;
  App.monaco = monaco;
  editor.onDidChangeCursorPosition((e) => { $('st-pos').textContent = `Ln ${e.position.lineNumber}, Col ${e.position.column}`; renderBreadcrumbs(); });
  editor.onMouseDown((e) => { if (e.target.type === monaco.editor.MouseTargetType.GUTTER_GLYPH_MARGIN) toggleBreakpoint(e.target.position.lineNumber); });
  editor.onDidChangeModel(() => { const t = S.active && S.tabs.get(S.active); $('st-lang').textContent = t && !/\.coy( |$)/i.test(t.name) ? 'Plain text' : 'Coyote'; });
  monaco.editor.registerEditorOpener({
    openCodeEditor(_source, resource, sel) {
      const key = S.byUri.get(resource.toString());
      const line = sel && (sel.startLineNumber || sel.lineNumber);
      if (resource.scheme === 'coyote-lib') { openVirtual(resource, line); return true; }
      if (key && S.tabs.has(key)) { openFile(key, line); return true; }
      if (resource.scheme === 'file') { openFile(S.byUri.get(resource.toString()) || resource.fsPath, line); return true; }
      return false;
    },
  });
  editor.addCommand(monaco.KeyMod.CtrlCmd | monaco.KeyCode.KeyG, () => editor.getAction('editor.action.gotoLine').run());
}

function applyTheme() { window.CoyoteThemes.applyAppearance(S.info.settings); }

Object.assign(App, { loadLibs, loadWorkspace, indexFile, renderTabs, activate, openFile, openVirtual, openLibSource, closeTab, closeOthers, closeAll, saveFile, saveAll,
  runCheck, scheduleCheck, renderProblems, problemList, refreshBreakpointDecor, toggleBreakpoint, refreshDebugLine, bpSet, initMonaco, applyTheme, langHost, renderBreadcrumbs });

'use strict';
/* global studio */
// Side views: Explorer, Search, Libraries, Run & Debug lists, plus the command palette and quick open.

// ---------- view switching ----------
function showView(name) {
  if (S.view === name && S.sidebarVisible) { toggleSidebar(); return; }
  S.view = name;
  if (!S.sidebarVisible) toggleSidebar(true);
  for (const b of document.querySelectorAll('#activitybar button[data-view]')) b.classList.toggle('active', b.dataset.view === name);
  for (const v of document.querySelectorAll('#sidepanel .view')) v.classList.toggle('hidden', v.id !== 'view-' + name);
  if (name === 'search') { $('search-q').focus(); $('search-q').select(); }
  if (name === 'libs') renderLibsView();
  if (name === 'project') App.renderProjectView();
}
function toggleSidebar(force) {
  S.sidebarVisible = force === undefined ? !S.sidebarVisible : force;
  $('sidepanel').classList.toggle('hidden', !S.sidebarVisible);
  $('resizer-v').classList.toggle('hidden', !S.sidebarVisible);
  if (!S.sidebarVisible) for (const b of document.querySelectorAll('#activitybar button[data-view]')) b.classList.remove('active');
  else for (const b of document.querySelectorAll('#activitybar button[data-view]')) b.classList.toggle('active', b.dataset.view === S.view);
}

// ---------- explorer ----------
function fileIcon(it) {
  if (it.dir) return '<span class="ico dir"><svg viewBox="0 0 16 16" style="width:14px;height:14px"><path d="M1.5 3.5h5l1.5 1.5h6.5v8h-13z" fill="currentColor"/></svg></span>';
  const n = it.name.toLowerCase();
  if (n.endsWith('.coy')) return '<span class="ico coy">◆</span>';
  if (n.endsWith('.lib')) return '<span class="ico lib">▤</span>';
  if (n.endsWith('.cyproj')) return '<span class="ico json">◈</span>';
  if (n.endsWith('.json')) return '<span class="ico json">{}</span>';
  if (n.endsWith('.md')) return '<span class="ico md">M</span>';
  return '<span class="ico">·</span>';
}
async function renderTree() {
  const el = $('tree');
  const scroll = el.scrollTop;
  el.innerHTML = '';
  $('tree-empty').classList.toggle('hidden', !!S.root);
  if (!S.root) return;
  async function walk(dir, depth) {
    const items = await studio.fs.list(dir);
    for (const it of items) {
      const row = document.createElement('div');
      row.className = 'node' + (S.selected === it.path ? ' active' : '');
      row.style.paddingLeft = 8 + depth * 14 + 'px';
      row.dataset.path = it.path;
      const special = !it.dir && (it.name === 'packages.json' || it.name.toLowerCase().endsWith('.cyproj')) ? '<span class="badge">project</span>' : '';
      row.innerHTML = `<span class="twist">${it.dir ? (S.expanded.has(it.path) ? '▾' : '▸') : ''}</span>${fileIcon(it)}<span>${esc(it.name)}</span>${special}`;
      row.onclick = async () => {
        S.selected = it.path;
        if (it.dir) { S.expanded.has(it.path) ? S.expanded.delete(it.path) : S.expanded.add(it.path); await renderTree(); } else await App.openFile(it.path);
        highlightTree();
      };
      row.oncontextmenu = (e) => { e.preventDefault(); S.selected = it.path; treeMenu(e.clientX, e.clientY, it); };
      el.append(row);
      if (it.dir && S.expanded.has(it.path)) await walk(it.path, depth + 1);
    }
  }
  await walk(S.root, 0);
  el.scrollTop = scroll;
}
function highlightTree() {
  const t = S.active && S.tabs.get(S.active);
  for (const n of document.querySelectorAll('#tree .node')) n.classList.toggle('active', n.dataset.path === ((t && t.path) || S.selected));
}
function treeMenu(x, y, it) {
  const folder = it.dir ? it.path : dirOf(it.path);
  App.ctxMenu(x, y, [
    { label: 'New File...', run: () => newFile(folder) }, { label: 'New Folder...', run: () => newFolder(folder) }, '-',
    { label: 'Rename...', run: async () => {
      const n = await App.prompt('Rename', 'New name', it.name);
      if (!n) return;
      const r = await studio.fs.rename(it.path, join(dirOf(it.path), n));
      if (r.ok === false) return App.toast('rename: ' + r.error, 'error');
      if (S.tabs.has(it.path)) { S.tabs.get(it.path).dirty = false; await App.closeTab(it.path); }
      await renderTree(); await App.loadWorkspace();
    } },
    { label: 'Delete', run: async () => {
      if ((await studio.dialog.confirm(`Delete ${it.name}?`, 'This cannot be undone.', ['Delete', 'Cancel'])) !== 0) return;
      await studio.fs.remove(it.path);
      for (const p of [...S.tabs.keys()]) if (p === it.path || p.startsWith(it.path + sepOf())) { S.tabs.get(p).dirty = false; await App.closeTab(p); }
      await renderTree(); await App.loadWorkspace();
    } }, '-',
    { label: 'Reveal in File Manager', run: () => studio.fs.reveal(it.path) },
    { label: 'Copy Path', run: () => navigator.clipboard && navigator.clipboard.writeText(it.path) },
  ]);
}
const TEMPLATE = 'use "Standard"\n\nShell.PrintLine("Hello from Coyote")\n';
async function newFile(dir) {
  dir = dir || S.root;
  if (!dir) return App.toast('Open a folder first.', 'error');
  let n = await App.prompt('New file', 'File name', 'untitled.coy');
  if (!n) return;
  if (!/\.[A-Za-z0-9]+$/.test(n)) n += '.coy';
  const p = join(dir, n);
  const r = await studio.fs.create(p, n.endsWith('.coy') ? TEMPLATE : '');
  if (r.ok === false) return App.toast('new file: ' + r.error, 'error');
  S.expanded.add(dir);
  await renderTree();
  await App.openFile(p);
}
async function newFolder(dir) {
  dir = dir || S.root;
  if (!dir) return;
  const n = await App.prompt('New folder', 'Folder name', 'src');
  if (!n) return;
  await studio.fs.mkdir(join(dir, n));
  S.expanded.add(dir);
  await renderTree();
}

// ---------- search ----------
const sq = { case: false, word: false, regex: false };
let searchSeq = 0;
async function runSearch() {
  const q = $('search-q').value;
  const out = $('search-results');
  if (!q || !S.root) { out.innerHTML = ''; $('search-summary').textContent = ''; return; }
  const my = ++searchSeq;
  const r = await studio.search.text(S.root, q, { caseSensitive: sq.case, wholeWord: sq.word, regex: sq.regex, include: $('search-inc').value });
  if (my !== searchSeq) return;
  out.innerHTML = '';
  if (r.error) { $('search-summary').textContent = r.error; return; }
  const byFile = new Map();
  for (const h of r.results) { if (!byFile.has(h.file)) byFile.set(h.file, []); byFile.get(h.file).push(h); }
  $('search-summary').textContent = `${r.results.length}${r.truncated ? '+' : ''} results in ${byFile.size} files`;
  for (const [file, hits] of byFile) {
    const f = document.createElement('div');
    f.className = 'sr-file';
    f.innerHTML = `<span>${esc(base(file))}</span><span class="muted small">${esc(norm(file).slice(norm(S.root).length + 1))} (${hits.length})</span>`;
    out.append(f);
    for (const h of hits) {
      const d = document.createElement('div');
      d.className = 'sr-hit';
      const t = h.text.trimStart();
      const cut = h.text.length - t.length;
      const s = Math.max(0, h.column - 1 - cut);
      d.innerHTML = `${esc(t.slice(0, s))}<mark>${esc(t.slice(s, s + h.length))}</mark>${esc(t.slice(s + h.length))}`;
      d.title = `${file}:${h.line}`;
      d.onclick = async () => { await App.openFile(file, h.line, h.column); editor.setSelection(new App.monaco.Range(h.line, h.column, h.line, h.column + h.length)); };
      out.append(d);
    }
  }
}

// ---------- libraries view ----------
function renderLibsView() {
  const el = $('libs-tree');
  if (!el) return;
  el.innerHTML = '';
  const libs = [...S.libs.values()].sort((a, b) => a.name.localeCompare(b.name));
  $('libs-info').textContent = libs.length ? `${libs.length} libraries found. Click a symbol to read its source.` : 'No libraries found. The installer puts them in the "libs" folder next to the Coyote tools; your own go to ~/coyote/libs.';
  const row = (depth, kind, label, sig, onClick, title) => {
    const d = document.createElement('div');
    d.className = 'lib-row';
    d.style.paddingLeft = 8 + depth * 14 + 'px';
    d.innerHTML = `<span class="k ${kind}">${{ lib: '▤', class: 'C', method: 'm', property: 'p', field: 'f' }[kind] || '·'}</span><span>${esc(label)}</span><span class="sig">${esc(sig || '')}</span>`;
    if (title) d.title = title;
    d.onclick = onClick;
    return d;
  };
  for (const lib of libs) {
    const wrap = document.createElement('div');
    wrap.className = 'lib-node';
    const kids = document.createElement('div');
    kids.className = 'hidden';
    const head = row(0, 'lib', lib.name, lib.requires.length ? `includes ${lib.requires.length}` : `v${lib.version}`, () => kids.classList.toggle('hidden'), `${lib.path}\nversion ${lib.version}` + (lib.requires.length ? `\nincludes: ${lib.requires.join(', ')}` : ''));
    wrap.append(head, kids);
    for (const r of lib.requires) kids.append(row(1, 'lib', r, 'included', () => { const l = S.libs.get(r.toLowerCase()); if (l) { const f = l.files[0]; f && App.openLibSource(l.name, f, 1); } }));
    for (const c of lib.classes) {
      const ck = document.createElement('div');
      ck.className = 'hidden';
      const ch = row(1, 'class', c.name, c.kind, (e) => { if (e.detail === 2) App.openLibSource(lib.name, c.file, c.line); else ck.classList.toggle('hidden'); }, c.doc);
      kids.append(ch, ck);
      for (const m of c.members) {
        if (m.private || m.kind === 'ctor') continue;
        ck.append(row(2, m.kind, m.name, L.signatureOf(m).replace(/^static /, ''), () => App.openLibSource(lib.name, c.file, m.line), m.doc));
      }
    }
    for (const m of lib.methods) kids.append(row(1, 'method', m.name, L.signatureOf(m), () => App.openLibSource(lib.name, m.file, m.line), m.doc));
    el.append(wrap);
  }
}

// ---------- run & debug lists ----------
function row(html, cls) { const d = document.createElement('div'); d.className = 'row ' + (cls || ''); d.innerHTML = html; return d; }
function renderBps() {
  const el = $('bps');
  el.innerHTML = '';
  let n = 0;
  for (const [f, set] of S.bps) for (const l of [...set].sort((a, b) => a - b)) {
    n++;
    const d = row(`<span style="color:#e51400">●</span> ${esc(base(f))}:${l}`);
    d.style.cursor = 'pointer';
    d.onclick = () => App.openFile(f, l);
    el.append(d);
  }
  if (!n) el.innerHTML = '<div class="empty">none (click the left margin of the editor)</div>';
}

// ---------- command palette / quick open ----------
function paletteCommands() {
  return Object.entries(App.COMMANDS).map(([id, c]) => ({ id, ...c }));
}
function fileItems(q) {
  const out = [];
  for (const e of S.ws.values()) {
    const rel = S.root ? norm(e.path).slice(norm(S.root).length + 1) : e.path;
    const f = fuzzy(q, rel);
    if (f) out.push({ score: f.score, html: f.html, label: rel, detail: '', run: () => App.openFile(e.path) });
  }
  for (const t of S.tabs.values()) if (t.path && !S.ws.has(keyOf(t.path))) { const f = fuzzy(q, t.name); if (f) out.push({ score: f.score, html: f.html, label: t.name, detail: '', run: () => App.openFile(t.path) }); }
  return out.sort((a, b) => a.score - b.score);
}
function symbolItems(q, scopeWorkspace) {
  const out = [];
  const push = (s, where, run) => { const f = fuzzy(q, s.name); if (f) out.push({ score: f.score, html: f.html, label: s.name, detail: `${L.signatureOf(s)}  ${where}`, run }); };
  if (!scopeWorkspace) {
    const t = S.active && S.tabs.get(S.active);
    if (!t) return [];
    const ix = L.indexSource(t.model.getValue());
    const goto = (line) => () => { editor.revealLineInCenter(line); editor.setPosition({ lineNumber: line, column: 1 }); editor.focus(); };
    for (const c of ix.classes) { push(c, '', goto(c.line)); for (const m of c.members) push(m, c.name, goto(m.line)); }
    for (const m of ix.methods) push(m, '', goto(m.line));
    return out.sort((a, b) => a.score - b.score);
  }
  for (const e of S.ws.values()) {
    const where = base(e.path);
    for (const c of e.ix.classes) { push(c, where, () => App.openFile(e.path, c.line)); for (const m of c.members) push(m, `${c.name} - ${where}`, () => App.openFile(e.path, m.line)); }
    for (const m of e.ix.methods) push(m, where, () => App.openFile(e.path, m.line));
  }
  for (const lib of S.libs.values()) {
    for (const c of lib.classes) { push(c, lib.name, () => App.openLibSource(lib.name, c.file, c.line)); for (const m of c.members) if (!m.private && m.kind !== 'ctor') push(m, `${c.name} - ${lib.name}`, () => App.openLibSource(lib.name, c.file, m.line)); }
    for (const m of lib.methods) push(m, lib.name, () => App.openLibSource(lib.name, m.file, m.line));
  }
  return out.sort((a, b) => a.score - b.score).slice(0, 200);
}
function quickProvider(query) {
  if (query.startsWith('>')) {
    const q = query.slice(1).trim();
    return paletteCommands().map((c) => { const f = fuzzy(q, c.label); return f && { score: f.score, html: f.html, label: c.label, detail: c.key || '', run: c.run }; }).filter(Boolean).sort((a, b) => a.score - b.score);
  }
  if (query.startsWith('@')) return symbolItems(query.slice(1).trim(), false);
  if (query.startsWith('#')) return symbolItems(query.slice(1).trim(), true);
  if (query.startsWith(':')) {
    const n = parseInt(query.slice(1), 10);
    return n > 0 ? [{ label: `Go to line ${n}`, detail: '', run: () => { editor.revealLineInCenter(n); editor.setPosition({ lineNumber: n, column: 1 }); editor.focus(); } }] : [{ label: 'Type a line number', detail: '', run() {} }];
  }
  return fileItems(query);
}
function openPalette(prefix) { App.Quick.open(quickProvider, prefix || '>'); }
function openQuickOpen() { App.Quick.open(quickProvider, ''); }
function wireQuick() {
  const input = $('quick-input');
  input.addEventListener('input', () => App.Quick.refresh());
  input.addEventListener('keydown', (e) => {
    if (e.key === 'ArrowDown') { e.preventDefault(); App.Quick.move(1); } else if (e.key === 'ArrowUp') { e.preventDefault(); App.Quick.move(-1); } else if (e.key === 'Enter') { e.preventDefault(); App.Quick.accept(); } else if (e.key === 'Escape') { e.preventDefault(); App.Quick.close(); }
  });
  $('overlay').addEventListener('mousedown', (e) => { if (e.target === $('overlay')) App.Quick.close(); });
}

Object.assign(App, { showView, toggleSidebar, renderTree, highlightTree, newFile, newFolder, runSearch, sq, renderLibsView, renderBps, row, openPalette, openQuickOpen, wireQuick, quickProvider, symbolItems });

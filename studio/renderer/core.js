'use strict';
/* global studio */
// Shared state and small UI helpers (dialogs, toasts, context menu, quick-pick overlay) for the Studio renderer.
// The other renderer scripts add their functions to the global `App`.
const App = {
  S: {
    root: null,            // opened folder
    project: null,         // folder holding packages.json (or null)
    manifest: null,
    tabs: new Map(),       // key -> { key, path, model, dirty, saved, name, readOnly, viewState, lib }
    byUri: new Map(),      // model uri string -> tab key
    active: null,
    bps: new Map(),        // path -> Set(line)
    bpDecor: new Map(),
    info: null,
    running: false,
    runKind: 'run',
    debug: { active: false, paused: false, file: null, line: 0, frame: 0, stack: [], wantsInput: false, queue: [] },
    expanded: new Set(),
    selected: null,
    markers: new Map(),    // checked path -> Map(file -> diagnostics)
    libs: new Map(),       // lowercase name -> library index entry (langindex.buildLibIndex)
    libDirs: [],
    ws: new Map(),         // normalized path -> { path, ix, text }
    gen: 0,                // bumped when libraries or workspace symbols change
    view: 'explorer',
    sidebarVisible: true,
  },
};

const $ = (id) => document.getElementById(id);
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const sepOf = () => (App.S.info && App.S.info.platform === 'win32' ? '\\' : '/');
const join = (a, b) => (a.endsWith('/') || a.endsWith('\\') ? a + b : a + sepOf() + b);
const base = (p) => String(p).split(/[\\/]/).pop();
const dirOf = (p) => { const i = Math.max(p.lastIndexOf('/'), p.lastIndexOf('\\')); return i < 0 ? '.' : p.slice(0, i); };
const norm = (p) => p.replace(/\\/g, '/');
const isAbs = (p) => /^([A-Za-z]:[\\/]|\/)/.test(p);
const keyOf = (p) => (App.S.info && App.S.info.platform === 'win32' ? norm(p).toLowerCase() : norm(p));
const cwdFor = (file) => App.S.project || dirOf(file);
function resolvePath(p, cwd) { return isAbs(p) ? p : join(cwd || App.S.project || App.S.root || '.', p); }

// Resolves `.` and `..` segments of a '/'-separated absolute path.
function cleanPath(p) {
  const parts = norm(p).split('/');
  const out = [];
  for (const s of parts) { if (s === '..') { if (out.length > 1) out.pop(); } else if (s !== '.') out.push(s); }
  return out.join('/');
}
function relativeSpec(fromFile, toFile) {
  const a = cleanPath(dirOf(norm(fromFile))).split('/');
  const b = cleanPath(norm(toFile)).split('/');
  let i = 0;
  while (i < a.length && i < b.length - 1 && a[i].toLowerCase() === b[i].toLowerCase()) i++;
  const ups = a.length - i;
  return (ups ? '../'.repeat(ups) : '') + b.slice(i).join('/');
}

// ---------- toasts ----------
function toast(text, kind, ms) {
  const el = document.createElement('div');
  el.className = 'toast ' + (kind || '');
  el.textContent = text;
  $('toasts').append(el);
  setTimeout(() => el.remove(), ms || 4200);
}

// ---------- modal ----------
function modal(html, buttons, { onOpen } = {}) {
  return new Promise((resolve) => {
    const box = $('modal-box');
    box.innerHTML = html + '<div class="buttons"></div>';
    const bar = box.querySelector('.buttons');
    const close = (v) => { $('modal').classList.add('hidden'); document.removeEventListener('keydown', onKey, true); resolve(v); };
    for (const b of buttons) {
      const el = document.createElement('button');
      el.textContent = b.label;
      if (b.primary) el.className = 'primary';
      el.onclick = () => { const v = b.value ? b.value(box) : b.label; if (v === false) return; close(v === undefined ? b.label : v); };
      bar.append(el);
    }
    function onKey(e) {
      if (e.key === 'Escape') { e.stopPropagation(); close(null); }
      if (e.key === 'Enter' && e.target && e.target.tagName === 'INPUT') { e.preventDefault(); const p = bar.querySelector('.primary'); if (p) p.click(); }
    }
    document.addEventListener('keydown', onKey, true);
    $('modal').classList.remove('hidden');
    if (onOpen) onOpen(box);
    const first = box.querySelector('input[type=text]');
    if (first) { first.focus(); first.select(); }
  });
}
async function prompt(title, label, value = '') {
  const r = await modal(`<h3>${esc(title)}</h3><label>${esc(label)}</label><input id="prompt-input" type="text" value="${esc(value)}">`,
    [{ label: 'OK', primary: true, value: (b) => b.querySelector('#prompt-input').value.trim() }, { label: 'Cancel', value: () => null }]);
  return r || null;
}

// ---------- context menu ----------
function ctxMenu(x, y, entries) {
  const m = $('ctxmenu');
  m.innerHTML = '';
  for (const e of entries) {
    if (e === '-') { m.append(document.createElement('hr')); continue; }
    const d = document.createElement('div');
    d.textContent = e.label;
    d.onclick = () => { m.classList.add('hidden'); e.run(); };
    m.append(d);
  }
  m.style.left = Math.min(x, window.innerWidth - 220) + 'px';
  m.style.top = Math.min(y, window.innerHeight - entries.length * 28 - 10) + 'px';
  m.classList.remove('hidden');
}
document.addEventListener('click', () => $('ctxmenu').classList.add('hidden'));

// ---------- quick pick overlay (command palette, quick open, symbols) ----------
// provider(query) -> [{ label, detail, run(), matches?: [indexes] }]
const Quick = {
  items: [], sel: 0, provider: null,
  open(provider, initial) {
    this.provider = provider;
    $('overlay').classList.remove('hidden');
    const input = $('quick-input');
    input.value = initial || '';
    input.focus();
    this.refresh();
  },
  close() { $('overlay').classList.add('hidden'); this.provider = null; if (App.editor) App.editor.focus(); },
  refresh() {
    this.items = this.provider ? this.provider($('quick-input').value) : [];
    this.sel = 0;
    this.render();
  },
  render() {
    const list = $('quick-list');
    list.innerHTML = '';
    this.items.slice(0, 200).forEach((it, i) => {
      const d = document.createElement('div');
      d.className = 'qi' + (i === this.sel ? ' sel' : '');
      d.innerHTML = `<span>${it.html || esc(it.label)}</span><span class="d">${esc(it.detail || '')}</span>`;
      d.onclick = () => this.accept(i);
      list.append(d);
    });
    if (!this.items.length) list.innerHTML = '<div class="qi"><span class="muted">No matches</span></div>';
  },
  move(delta) {
    if (!this.items.length) return;
    this.sel = (this.sel + delta + Math.min(this.items.length, 200)) % Math.min(this.items.length, 200);
    this.render();
    const el = $('quick-list').children[this.sel];
    if (el) el.scrollIntoView({ block: 'nearest' });
  },
  accept(i) {
    const it = this.items[i === undefined ? this.sel : i];
    if (!it) return;
    this.close();
    it.run();
  },
};
// Subsequence match: returns { score, html } or null.
function fuzzy(query, text) {
  if (!query) return { score: 0, html: esc(text) };
  const q = query.toLowerCase(), t = text.toLowerCase();
  let ti = 0, score = 0, last = -2;
  const hit = new Set();
  for (const ch of q) {
    const at = t.indexOf(ch, ti);
    if (at < 0) return null;
    score += at === last + 1 ? 0 : 1 + (at - ti);
    if (at === 0 || /[\s_\-./\\]/.test(text[at - 1]) || (text[at] >= 'A' && text[at] <= 'Z')) score -= 2;
    hit.add(at);
    last = at;
    ti = at + 1;
  }
  let html = '';
  for (let i = 0; i < text.length; i++) html += hit.has(i) ? `<mark>${esc(text[i])}</mark>` : esc(text[i]);
  return { score: score + text.length * 0.01, html };
}

Object.assign(App, { $, esc, sepOf, join, base, dirOf, norm, isAbs, keyOf, cwdFor, resolvePath, cleanPath, relativeSpec, toast, modal, prompt, ctxMenu, Quick, fuzzy });

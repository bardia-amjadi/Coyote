'use strict';
/* global studio */
// Coyote Studio renderer: wiring of menu, commands, dialogs, settings and start-up. The pieces live in
// core.js (helpers), editor.js (Monaco, tabs, diagnostics), views.js (side views, palette), run.js (run/debug).

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
  await App.renderTree();
  await Promise.all([App.loadWorkspace(), App.loadLibs()]);
  const entry = r.manifest && r.manifest.ok && r.manifest.entry ? join(r.root, r.manifest.entry) : null;
  if (entry) await App.openFile(entry).catch(() => {});
  updateProjectUi();
  App.setState(r.root ? 'Project: ' + base(r.root) : 'Folder opened (no project file)');
  refreshRecent();
}

function updateProjectUi() {
  const ok = !!(S.manifest && S.manifest.ok);
  $('btn-build').title = ok ? 'Build the project (F7). What it produces is set in the project file.' : 'Build (needs a project file)';
  App.renderProjectView();
  App.snapshotProps();
}
App.updateProjectUi = updateProjectUi;

async function newProject() {
  const home = S.info.home;
  const defLoc = S.info.settings.lastProjectDir || join(join(home, 'coyote'), 'projects');
  let kind = 'app';
  const r = await App.modal(`<h3>New project</h3>
    <div class="templates">
      <div class="tpl sel" data-kind="app"><b>Console App</b><span>A program that runs and talks to the terminal. Uses the Standard libraries.</span></div>
      <div class="tpl" data-kind="library"><b>Library</b><span>A reusable .lib that other programs bring in with <code>use</code>.</span></div>
    </div>
    <label>Name (letters, digits, _ - .)</label><input id="np-name" type="text" value="my-app">
    <label>Location</label><div class="row2"><input id="np-loc" type="text" value="${esc(defLoc)}"><button id="np-browse">Browse...</button></div>`,
  [{ label: 'Create', primary: true, value: (b) => ({ kind, name: b.querySelector('#np-name').value.trim(), loc: b.querySelector('#np-loc').value.trim() }) }, { label: 'Cancel', value: () => null }],
  { onOpen: (box) => {
    for (const t of box.querySelectorAll('.tpl')) t.onclick = () => { kind = t.dataset.kind; for (const x of box.querySelectorAll('.tpl')) x.classList.toggle('sel', x === t); const n = box.querySelector('#np-name'); if (/^my-(app|lib)$/.test(n.value)) n.value = kind === 'library' ? 'my-lib' : 'my-app'; };
    box.querySelector('#np-browse').onclick = async () => { const d = await studio.dialog.openFolder(); if (d) box.querySelector('#np-loc').value = d; };
  } });
  if (!r || !r.name || !r.loc) return;
  await studio.setSetting('lastProjectDir', r.loc);
  const res = await studio.project.create(r.loc, r.name, r.kind);
  if (!res.ok) return App.toast('new project: ' + res.error, 'error');
  await openFolder(res.dir);
}

async function refreshRecent() {
  S.info = await studio.info();
  const el = $('recent');
  el.innerHTML = '';
  for (const d of S.info.settings.recent || []) {
    const a = document.createElement('a');
    a.innerHTML = `${esc(base(d))}<small>${esc(d)}</small>`;
    a.onclick = () => openFolder(d);
    el.append(a);
  }
  if (!el.children.length) el.innerHTML = '<span class="muted">No recent folders.</span>';
}

// ---------- dialogs ----------
function applySettings() {
  const s = S.info.settings;
  const d = (k, v) => (s[k] === undefined ? v : s[k]);
  App.applyTheme();
  if (!editor) return;
  editor.updateOptions({
    fontSize: s.fontSize || 14, fontFamily: s.fontFamily || 'Cascadia Mono, Consolas, monospace', fontLigatures: !!d('fontLigatures', false), lineHeight: d('lineHeight', 0) || 0,
    tabSize: s.tabSize || 4, minimap: { enabled: !!s.minimap }, wordWrap: s.wordWrap ? 'on' : 'off', lineNumbers: d('lineNumbers', 'on'), renderWhitespace: d('renderWhitespace', 'selection'),
    cursorStyle: d('cursorStyle', 'line'), cursorBlinking: d('cursorBlinking', 'blink'), smoothScrolling: !!d('smoothScrolling', true), bracketPairColorization: { enabled: !!d('bracketPairs', true) },
    guides: { bracketPairs: !!d('bracketPairs', true), indentation: !!d('indentGuides', true) }, stickyScroll: { enabled: !!d('stickyScroll', true) },
    quickSuggestions: d('quickSuggestions', true) ? { other: true, comments: false, strings: true } : false, acceptSuggestionOnEnter: d('acceptOnEnter', 'on'), parameterHints: { enabled: !!d('parameterHints', true) },
  });
}
App.applySettings = applySettings;
App.updateToolchainStatus = () => updateToolchainStatus();
async function toggleTheme() {
  const next = S.info.settings.theme === 'light' ? 'dark' : 'light';
  await studio.setSetting('theme', next);
  S.info = await studio.info();
  applySettings();
}
async function showAbout() {
  const v = await studio.versions();
  const info = await studio.info();
  const lines = Object.entries(v).map(([k, val]) => `${k.padEnd(12)} ${val || 'not installed'}`).join('\n');
  await App.modal(`<h3>Coyote Studio ${esc(info.version)}</h3><pre>${esc(lines)}</pre><p class="muted">Toolchain: ${esc(info.toolchain.dir || 'not found')}</p><p class="muted">Libraries: ${esc(S.libDirs.join('\n'))}</p>`, [{ label: 'Close', primary: true }]);
}
async function showDoctor() {
  const r = await studio.doctor();
  await App.modal(`<h3>Toolchain doctor</h3><pre>${esc(r.text || r.error || '')}</pre>`, [{ label: 'Close', primary: true }]);
}
function updateToolchainStatus() {
  const tc = S.info.toolchain;
  $('st-tool').textContent = tc.dir ? 'Coyote ' + (S.info.version || '') : 'Coyote toolchain NOT FOUND';
  $('st-tool').title = tc.dir || '';
  const w = $('toolchain-warning');
  if (!tc.dir) { w.textContent = 'The Coyote tools (coyote, pvm) were not found. Install Coyote with the setup program, or set the toolchain folder in Settings.'; w.classList.remove('hidden'); } else if (tc.missing.length) { w.textContent = 'Some tools are missing: ' + tc.missing.join(', '); w.classList.remove('hidden'); } else w.classList.add('hidden');
}

// ---------- commands (menu, palette) ----------
const fmt = () => (App.activeCoy() ? editor.getAction('editor.action.formatDocument').run() : null);
const MENU = {
  newProject, newFile: () => App.newFile(), openFolder: () => openFolder(), openFile: async () => { const f = await studio.dialog.openFile(); if (f) App.openFile(f); },
  save: () => S.active && App.saveFile(S.active), saveAll: App.saveAll, format: fmt, closeTab: () => S.active && App.closeTab(S.active), closeAll: App.closeAll,
  run: App.runActive, runProject: App.runProject, debug: App.startDebug, stop: () => studio.run.stop(),
  continue: () => S.debug.active && App.debugCmd('resume'), stepOver: () => S.debug.active && App.debugCmd('stepOver'), stepInto: () => S.debug.active && App.debugCmd('stepInto'), stepOut: () => S.debug.active && App.debugCmd('stepOut'),
  toggleBreakpoint: () => App.toggleBreakpoint(),
  check: async () => { const f = App.activeCoy(); if (f) { await App.saveAll(); await App.runCheck(f); App.showTab('problems'); } },
  showMl: App.showMl, build: () => App.buildProject('build'), rebuild: () => App.buildProject('rebuild'), clean: () => App.cleanProject(false), openOutput: App.openOutputFolder, viewProject: () => App.showView('project'), openProject: async () => { const d = await studio.dialog.openProject(); if (d) openFolder(d); }, packLib: () => App.packLib(false), installLib: () => App.packLib(true), cpmInstall: App.cpmInstall, doctor: showDoctor, settings: () => App.openSettings(), about: showAbout,
  palette: () => App.openPalette('>'), quickOpen: () => App.openQuickOpen(), goSymbol: () => App.openPalette('@'), goSymbolWorkspace: () => App.openPalette('#'),
  viewExplorer: () => App.showView('explorer'), viewSearch: () => App.showView('search'), viewDebug: () => App.showView('debug'), viewLibs: () => App.showView('libs'),
  toggleSidebar: () => App.toggleSidebar(), togglePanel: () => App.togglePanel(), terminal: () => App.showTab('terminal'), toggleTheme,
  libFolder: async () => { const r = await studio.shell.openPath(join(join(S.info.home, 'coyote'), 'libs')); if (r && r.ok === false) App.toast(r.error, 'error'); },
  reloadLibs: async () => { await App.loadLibs(); App.toast('Libraries reloaded', 'good'); },
};
App.COMMANDS = {
  run: { label: 'Run: Start without debugging', key: 'F5', run: MENU.run }, debug: { label: 'Run: Start debugging', key: 'F6', run: MENU.debug }, stop: { label: 'Run: Stop', key: 'Shift+F5', run: MENU.stop },
  runProject: { label: 'Run: Run project (cpx start)', key: 'Ctrl+F5', run: MENU.runProject }, toggleBreakpoint: { label: 'Debug: Toggle breakpoint', key: 'F9', run: MENU.toggleBreakpoint },
  newProject: { label: 'File: New project...', key: 'Ctrl+Shift+N', run: MENU.newProject }, newFile: { label: 'File: New file', key: 'Ctrl+N', run: MENU.newFile },
  openFolder: { label: 'File: Open folder...', run: MENU.openFolder }, openFile: { label: 'File: Open file...', key: 'Ctrl+O', run: MENU.openFile }, save: { label: 'File: Save', key: 'Ctrl+S', run: MENU.save }, saveAll: { label: 'File: Save all', run: MENU.saveAll },
  closeTab: { label: 'View: Close editor', key: 'Ctrl+W', run: MENU.closeTab },
  format: { label: 'Format document', key: 'Shift+Alt+F', run: MENU.format }, check: { label: 'Tools: Check file', run: MENU.check }, showMl: { label: 'Tools: Show generated ML', run: MENU.showMl },
  build: { label: 'Build: Build project', key: 'F7', run: MENU.build }, rebuild: { label: 'Build: Rebuild project', key: 'Ctrl+Shift+F7', run: MENU.rebuild }, clean: { label: 'Build: Clean project', run: MENU.clean }, openOutput: { label: 'Build: Open output folder', run: MENU.openOutput }, projectView: { label: 'View: Project Manager', key: 'Ctrl+Shift+P', run: MENU.viewProject }, packLib: { label: 'Library: Build .lib', run: MENU.packLib }, installLib: { label: 'Library: Build and install for me', run: MENU.installLib },
  libFolder: { label: 'Library: Open my libraries folder', run: MENU.libFolder }, reloadLibs: { label: 'Library: Reload installed libraries', run: MENU.reloadLibs },
  cpm: { label: 'Packages: cpm install', run: MENU.cpmInstall }, doctor: { label: 'Tools: Toolchain doctor', run: MENU.doctor },
  explorer: { label: 'View: Explorer', key: 'Ctrl+Shift+E', run: MENU.viewExplorer }, search: { label: 'View: Find in files', key: 'Ctrl+Shift+F', run: MENU.viewSearch },
  debugView: { label: 'View: Run and Debug', key: 'Ctrl+Shift+D', run: MENU.viewDebug }, libsView: { label: 'View: Libraries', key: 'Ctrl+Shift+L', run: MENU.viewLibs },
  terminal: { label: 'View: Terminal', key: 'Ctrl+`', run: MENU.terminal }, panel: { label: 'View: Toggle panel', key: 'Ctrl+J', run: MENU.togglePanel }, sidebar: { label: 'View: Toggle sidebar', key: 'Ctrl+B', run: MENU.toggleSidebar },
  theme: { label: 'View: Toggle dark / light theme', run: MENU.toggleTheme }, settings: { label: 'Preferences: Settings', run: MENU.settings }, about: { label: 'Help: About Coyote Studio', run: MENU.about },
  gosym: { label: 'Go to symbol in file', key: 'Ctrl+Shift+O', run: MENU.goSymbol }, gosymws: { label: 'Go to symbol in project and libraries', key: 'Ctrl+T', run: MENU.goSymbolWorkspace },
  gotoline: { label: 'Go to line...', key: 'Ctrl+G', run: () => editor.getAction('editor.action.gotoLine').run() },
};

// ---------- layout ----------
function makeResizer(el, onDrag, cursorClass) {
  el.addEventListener('mousedown', (e) => {
    e.preventDefault();
    el.classList.add('drag');
    const move = (ev) => onDrag(ev);
    const up = () => { el.classList.remove('drag'); window.removeEventListener('mousemove', move); window.removeEventListener('mouseup', up); };
    window.addEventListener('mousemove', move);
    window.addEventListener('mouseup', up);
  });
}

async function init() {
  S.info = await studio.info();
  const st = S.info.settings;
  App.applyTheme();
  $('welcome-version').textContent = 'Version ' + S.info.version;
  updateToolchainStatus();
  App.wireQuick();
  App.initPanels();
  await refreshRecent();
  await App.initMonaco();
  applySettings();
  window.__studio = { S, monaco: App.monaco, editor, openFile: App.openFile, MENU, openFolderPath: openFolder, App };  // used by the automated UI test

  // toolbar and views
  $('btn-run').onclick = App.runActive; $('btn-debug').onclick = App.startDebug; $('btn-stop').onclick = () => studio.run.stop();
  $('btn-format').onclick = MENU.format; $('btn-build').onclick = () => App.buildProject('build'); App.wireProjectView();
  $('btn-palette').onclick = () => App.openQuickOpen(); $('btn-theme').onclick = toggleTheme; $('btn-settings').onclick = () => App.openSettings(); $('act-settings').onclick = () => App.openSettings();
  $('dbg-continue').onclick = MENU.continue; $('dbg-over').onclick = MENU.stepOver; $('dbg-into').onclick = MENU.stepInto; $('dbg-out').onclick = MENU.stepOut;
  for (const b of document.querySelectorAll('#activitybar button[data-view]')) b.onclick = () => App.showView(b.dataset.view);
  $('ex-newfile').onclick = () => App.newFile(); $('ex-newdir').onclick = () => App.newFolder(); $('ex-refresh').onclick = async () => { await App.renderTree(); await App.loadWorkspace(); };
  $('ex-collapse').onclick = () => { S.expanded = new Set(); App.renderTree(); };
  $('ex-open').onclick = () => openFolder(); $('ex-new').onclick = newProject;
  $('w-open').onclick = () => openFolder(); $('w-new').onclick = newProject; $('w-file').onclick = MENU.openFile; $('w-palette').onclick = MENU.palette;
  $('dv-start').onclick = App.startDebug;
  $('bp-clear').onclick = () => { for (const [f] of S.bps) { for (const l of S.bps.get(f)) if (S.debug.active) studio.debug.unbreak(f, l); } S.bps.clear(); for (const [p, ids] of S.bpDecor) { const t = S.tabs.get(p); if (t) t.model.deltaDecorations(ids, []); } S.bpDecor.clear(); App.renderBps(); };
  $('lib-folder').onclick = MENU.libFolder; $('lib-refresh').onclick = MENU.reloadLibs;
  $('st-problems').onclick = () => App.showTab('problems'); $('st-libs').onclick = () => App.showView('libs'); $('st-pos').onclick = () => editor.getAction('editor.action.gotoLine').run();
  $('panel-clear').onclick = () => { const act = document.querySelector('#panel-tabs button.active'); (act && act.dataset.tab === 'output' ? App.outTerm : App.term).clear(); };
  $('panel-close').onclick = App.togglePanel;
  $('term-eof').onclick = () => App.endInput();
  for (const b of document.querySelectorAll('#panel-tabs button[data-tab]')) b.onclick = () => App.showTab(b.dataset.tab);
  // search view
  const doSearch = () => App.runSearch();
  let st2; $('search-q').oninput = () => { clearTimeout(st2); st2 = setTimeout(doSearch, 250); }; $('search-inc').oninput = doSearch;
  $('search-q').onkeydown = (e) => { if (e.key === 'Enter') doSearch(); };
  for (const [id, key] of [['sq-case', 'case'], ['sq-word', 'word'], ['sq-regex', 'regex']]) $(id).onclick = () => { App.sq[key] = !App.sq[key]; $(id).classList.toggle('on', App.sq[key]); doSearch(); };
  // resizers
  if (st.sidebarWidth) $('sidepanel').style.width = st.sidebarWidth + 'px';
  if (st.panelHeight) $('panel').style.height = st.panelHeight + 'px';
  makeResizer($('resizer-v'), (e) => { const w = Math.min(600, Math.max(160, e.clientX - 46)); $('sidepanel').style.width = w + 'px'; });
  makeResizer($('resizer-h'), (e) => { const r = $('center').getBoundingClientRect(); const h = Math.min(r.height - 120, Math.max(80, r.bottom - e.clientY)); $('panel').style.height = h + 'px'; });
  window.addEventListener('beforeunload', () => { studio.setSetting('sidebarWidth', parseInt($('sidepanel').style.width, 10) || 280); studio.setSetting('panelHeight', parseInt($('panel').style.height, 10) || 260); });
  window.addEventListener('focus', () => { if (S.root) { App.renderTree(); App.loadWorkspace(); } });
  document.addEventListener('keydown', (e) => { if (e.key === 'F1') { e.preventDefault(); MENU.palette(); } }, true);

  // events from the main process
  studio.onMenu((a) => { if (MENU[a]) MENU[a](); });
  const openPath = (f) => (/\.cyproj$/i.test(f) ? openFolder(App.dirOf(f)) : App.openFile(f));
  studio.onOpenPath(openPath);
  studio.takeOpenPath().then((f) => { if (f) openPath(f); });
  studio.onRunOut(({ text }) => App.runSink().write(text));
  studio.onRunErr(({ text }) => App.runSink().write(text, 'err'));
  studio.onRunExit(App.onRunExit);
  studio.onDebug(App.onDebugEvent);

  App.showTab('terminal');
  App.renderBps();
  App.renderProblems();
  debugIdle();
  await App.loadLibs();
  if (!S.root) { $('tree-empty').classList.remove('hidden'); }
}
function debugIdle() { $('debug-idle').classList.remove('hidden'); $('debug-live').classList.add('hidden'); }

window.addEventListener('DOMContentLoaded', () => { init().catch((e) => { document.body.insertAdjacentHTML('beforeend', `<pre style="color:#f88;padding:20px;position:fixed;inset:0;background:#1e1e1e;z-index:999">Coyote Studio failed to start: ${esc(String(e && e.stack || e))}</pre>`); }); });

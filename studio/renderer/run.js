'use strict';
/* global studio, Terminal */
// Running, debugging and building: Terminal and Output panels, debugger UI, native/library builds.
let term = null;     // interactive Terminal panel
let outTerm = null;  // Output panel (tool messages)

function initPanels() {
  const link = (file, line, col) => App.openFile(resolvePath(file), line, col);
  term = new Terminal($('pane-terminal'), {
    onLine: (text) => {
      if (S.debug.active) {
        if (S.debug.wantsInput) { S.debug.wantsInput = false; term.setWaiting(false); studio.run.stdin(text + '\n'); } else S.debug.queue.push(text);
      } else studio.run.stdin(text + '\n');
    },
    onEof: () => endInput(),
    onInterrupt: () => { term.writeLine('^C', 'info'); studio.run.stop(); },
    onLink: link,
  });
  outTerm = new Terminal($('pane-output'), { onLink: link });
  mirrorToAppWindow(term);
  studio.onAppWin((ev) => {
    if (ev.type === 'line') term.h.onLine(ev.text);
    else if (ev.type === 'eof') endInput();
    else if (ev.type === 'stop') { term.writeLine('^C', 'info'); studio.run.stop(); } else if (ev.type === 'closed') { S.appWin = false; if (S.running) { term.writeLine('[app window closed: stopping the program]', 'info'); studio.run.stop(); } }
  });
}

// ---------- the app window ----------
// Setting "appWindow": 'debug' (default: only when debugging), 'always' (run and debug) or 'never' (Terminal panel only).
// The window mirrors everything the Terminal panel shows and sends typed lines back, so both stay in sync.
function mirrorToAppWindow(t) {
  const send = (op, a, b) => { if (S.appWin) studio.appwin.op({ op, a, b }); };
  const orig = { write: t.write.bind(t), writeLine: t.writeLine.bind(t), clear: t.clear.bind(t), setActive: t.setActive.bind(t), setWaiting: t.setWaiting.bind(t) };
  t.write = (x, c) => { orig.write(x, c); send('write', x, c); };
  t.writeLine = (x, c) => { orig.writeLine(x, c); send('writeLine', x, c); };
  t.clear = () => { orig.clear(); send('clear'); };
  t.setActive = (on) => { orig.setActive(on); send('active', on, S.debug.active ? 'debug' : 'run'); };
  t.setWaiting = (on) => { orig.setWaiting(on); send('waiting', on); };
}
async function openAppWindowFor(debug, title) {
  const mode = (S.info && S.info.settings && S.info.settings.appWindow) || 'debug';
  if (mode === 'never' || (mode === 'debug' && !debug)) return;
  const theme = document.body.classList.contains('theme-light') ? 'light' : 'dark';
  await studio.appwin.open(title, theme);
  S.appWin = true;
  studio.appwin.op({ op: 'clear' });
}
function endInput() {
  if (S.debug.active) {
    if (S.debug.wantsInput) { S.debug.wantsInput = false; term.setWaiting(false); studio.run.closeStdin(); } else S.debug.queue.push(null);
  } else studio.run.closeStdin();
  term.writeLine('^D', 'info');
}

// Output panel messages
function out(text, cls) { outTerm.write(text, cls); }
function outLine(text, cls) { outTerm.writeLine(text, cls); }

function showTab(name) {
  $('panel').classList.remove('collapsed');
  for (const b of document.querySelectorAll('#panel-tabs button[data-tab]')) b.classList.toggle('active', b.dataset.tab === name);
  for (const id of ['terminal', 'output', 'problems', 'ml']) $('pane-' + id).classList.toggle('hidden', id !== name);
  $('panel-clear').classList.toggle('hidden', name === 'problems' || name === 'ml');
  if (name === 'terminal') term.focus();
}
function togglePanel() { $('panel').classList.toggle('collapsed'); if (!$('panel').classList.contains('collapsed')) term.focus(); }

function setState(t) { $('st-state').textContent = t; }
function setRunning(v, label) {
  S.running = v;
  $('btn-stop').disabled = !v;
  for (const id of ['btn-run', 'btn-debug', 'btn-build']) $(id).disabled = v;
  term.setActive(v);
  $('term-eof').classList.toggle('hidden', !v);
  $('term-state').textContent = v ? (label || 'running') : '';
  setState(v ? (label || 'Running...') : 'Ready');
  if (S.appWin) studio.appwin.op({ op: 'state', a: v ? (label || 'running') : 'finished' });
  $('status').classList.toggle('debugging', v && S.debug.active);
  if (v) term.focus();
}
const activeCoy = () => { const t = S.active && S.tabs.get(S.active); return t && t.path && t.path.endsWith('.coy') ? t.path : null; };
const entryFile = () => (S.project && S.manifest && S.manifest.ok ? join(S.project, S.manifest.entry) : null);

async function runActive() {
  if ($('run-target').value === 'project') return runProject();
  const f = activeCoy() || entryFile();
  if (!f) return App.toast('Open a .coy file to run it.', 'error');
  await App.saveAll();
  showTab('terminal');
  term.clear();
  await openAppWindowFor(false, base(f));
  term.writeLine(`> run ${base(f)}`, 'info');
  setRunning(true, 'running ' + base(f));
  const r = await studio.run.file(f, cwdFor(f));
  if (r.ok === false) { term.writeLine(r.error, 'err'); setRunning(false); }
}
async function runProject() {
  if (!S.project) return App.toast('This folder has no project file (.cyproj). Use File > New Project, or run the current file.', 'error');
  if (S.manifest && S.manifest.manifest && S.manifest.manifest.type === 'library') return App.toast('This is a library project: it cannot run. Use Build, then `use` it from an app.', 'error');
  await App.saveAll();
  showTab('terminal');
  term.clear();
  await openAppWindowFor(false, base(S.project));
  term.writeLine(`> cpx start (${base(S.project)})`, 'info');
  setRunning(true, 'running project');
  const r = await studio.run.workflow(S.project, 'start');
  if (r.ok === false) { term.writeLine(r.error, 'err'); setRunning(false); }
}
async function packLib(install) {
  if (!S.project) return App.toast('This folder has no packages.json. Create a library with New Project (library).', 'error');
  await App.saveAll();
  showTab('output');
  outLine(`> coyote lib pack ${base(S.project)}${install ? ' (and install for the current user)' : ''}`, 'info');
  const r = await studio.lib.pack(S.project, install);
  if (!r.ok) { outLine(r.output || 'library build failed', 'err'); return App.toast('Library build failed', 'error'); }
  outLine(r.output, 'good');
  if (r.installedTo) { outLine(`Installed to ${r.installedTo}. Any program can now write: use "${base(r.file).replace(/\.lib$/, '')}"`, 'good'); await App.loadLibs(); App.toast('Library installed', 'good'); } else outLine(`Library written: ${r.file}. Use it with: use "./path/to/${base(r.file)}"`, 'good');
}
async function cpmInstall() {
  if (!S.project) return App.toast('This folder has no packages.json.', 'error');
  showTab('terminal');
  term.clear();
  term.writeLine('> cpm install', 'info');
  setRunning(true, 'cpm install');
  const r = await studio.run.cpm(S.project, ['install']);
  if (r.ok === false) { term.writeLine(r.error, 'err'); setRunning(false); }
}
async function showMl() {
  const f = activeCoy();
  if (!f) return;
  await App.saveAll();
  const r = await studio.tool.showMl(f, cwdFor(f));
  $('ml-text').textContent = r.text;
  showTab('ml');
}
// ---------- debugger ----------
async function loadFrame(i) {
  S.debug.frame = i;
  const vars = await studio.debug.vars(i);
  const el = $('vars');
  el.innerHTML = '';
  const list = (vars && vars.result) || [];
  for (const v of list) el.append(App.row(`<span class="name">${esc(v.name)}</span> <span class="type">${esc(v.type)}</span> = <span class="val">${esc(v.value)}</span>`));
  if (!list.length) el.innerHTML = '<div class="empty">no variables</div>';
  const g = await studio.debug.globals();
  const gl = $('globals');
  gl.innerHTML = '';
  for (const v of (g && g.result) || []) gl.append(App.row(`<span class="name">${esc(v.name)}</span> <span class="type">${esc(v.type)}</span> = <span class="val">${esc(v.value)}</span>`));
  if (!gl.children.length) gl.innerHTML = '<div class="empty">no globals</div>';
  [...$('stack').children].forEach((c, k) => c.classList.toggle('sel', k === i));
  const fr = S.debug.stack[i];
  if (fr && fr.line) { S.debug.file = resolvePath(fr.file); S.debug.line = fr.line; await App.openFile(S.debug.file, fr.line); App.refreshDebugLine(); }
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
  frames.forEach((f, i) => { const r = App.row(`${esc(f.function)} <span class="type">${esc(base(f.file))}:${f.line}</span>`, i === 0 ? 'sel' : ''); r.style.cursor = 'pointer'; r.onclick = () => loadFrame(i); el.append(r); });
  await App.openFile(S.debug.file, ev.line);
  await loadFrame(0);
  App.refreshDebugLine();
}
function debugUi(on) {
  S.debug.active = on;
  $('debugbar').classList.toggle('hidden', !on);
  $('debug-idle').classList.toggle('hidden', on);
  $('debug-live').classList.toggle('hidden', !on);
  $('status').classList.toggle('debugging', on);
  if (on) App.showView('debug');
  if (!on) { S.debug.paused = false; S.debug.wantsInput = false; S.debug.queue = []; term.setWaiting(false); App.refreshDebugLine(); $('vars').innerHTML = ''; $('stack').innerHTML = ''; $('globals').innerHTML = ''; }
}
function resumeUi() { S.debug.paused = false; App.refreshDebugLine(); setState('Running (debug)...'); }

async function startDebug() {
  const f = activeCoy() || entryFile();
  if (!f) return App.toast('Open a .coy file to debug it.', 'error');
  await App.saveAll();
  showTab('terminal');
  S.debug.active = true;  // before the window opens: its Terminal mirrors the debug flag
  await openAppWindowFor(true, base(f) + ' (debugging)');
  term.clear();
  term.writeLine(`> debug ${base(f)}`, 'info');
  const bps = [];
  for (const [file, set] of S.bps) for (const line of set) bps.push({ file, line });
  S.debug.active = true;
  S.debug.queue = [];
  S.debug.wantsInput = false;
  setRunning(true, 'debugging');
  const r = await studio.debug.start(f, cwdFor(f), bps);
  if (!r.ok) { S.debug.active = false; term.writeLine(r.error, 'err'); setRunning(false); return; }
  debugUi(true);
}
async function debugCmd(name) { resumeUi(); await studio.debug[name](); }

function onDebugEvent(ev) {
  if (ev.event === 'output') term.write(ev.text);
  else if (ev.event === 'log') term.write(ev.text, 'err');
  else if (ev.event === 'stopped') return onStopped(ev);
  else if (ev.event === 'inputRequested') {
    if (S.debug.queue.length) { const next = S.debug.queue.shift(); if (next === null) studio.run.closeStdin(); else studio.run.stdin(next + '\n'); } else { S.debug.wantsInput = true; term.setWaiting(true); term.focus(); setState('Waiting for input (type in the Terminal)'); }
  } else if (ev.event === 'runtimeError') { term.writeLine(`${ev.file}:${ev.line}: runtime error: ${ev.message}`, 'err'); return App.openFile(resolvePath(ev.file), ev.line); } else if (ev.event === 'exited') {
    term.writeLine(ev.code === 0 ? '[finished]' : `[exited with code ${ev.code}]`, ev.code === 0 ? 'good' : 'err');
    debugUi(false);
    setRunning(false);
  }
  return undefined;
}
function runSink() { return S.runKind === 'build' ? outTerm : term; }
function onRunExit(r) {
  if (S.runKind === 'build') return App.onBuildExit(r);
  term.writeLine(r.code === 0 ? '[finished]' : `[exited with code ${r.code}${r.error ? ': ' + r.error : ''}]`, r.code === 0 ? 'good' : 'err');
  setRunning(false);
}

Object.assign(App, { initPanels, out, outLine, showTab, togglePanel, setState, setRunning, runActive, runProject, packLib, cpmInstall, showMl, runSink, startDebug, debugCmd, debugUi, onDebugEvent, onRunExit, activeCoy, entryFile, endInput });
Object.defineProperty(App, 'term', { get: () => term });
Object.defineProperty(App, 'outTerm', { get: () => outTerm });

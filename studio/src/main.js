'use strict';
// Electron main process of Coyote Studio. All toolchain work happens here; the renderer talks to it through preload.js.
const { app, BrowserWindow, ipcMain, dialog, Menu, shell } = require('electron');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { findToolchain } = require('./toolchain');
const { Settings } = require('./settings');
const { findProjectRoot, readManifest, listDir } = require('./project');
const actions = require('./actions');
const { DebugSession } = require('./debug');
const search = require('./search');

const VERSION = require('../package.json').version;
let win = null;
let settings = null;
let tc = { dir: null, tools: {}, missing: [] };
let running = null;  // current run handle
let session = null;  // current DebugSession

function refreshToolchain() {
  tc = findToolchain({ settingsDir: settings.get('toolchainDir') });
  return tc;
}

function send(channel, payload) {
  if (win && !win.isDestroyed()) win.webContents.send(channel, payload);
}

// A file given on the command line (double-click on a .coy file, "Open with"). Packaged apps get the file as the last argument.
function pathFromArgv(argv, cwd) {
  const rest = argv.slice(app.isPackaged ? 1 : 2).filter((a) => a && !a.startsWith('-'));
  for (const a of rest) {
    const full = path.resolve(cwd || process.cwd(), a);
    try { if (fs.statSync(full).isFile()) return full; } catch { /* not a file */ }
  }
  return null;
}
let pendingOpen = null;

function createWindow() {
  win = new BrowserWindow({
    width: 1360, height: 860, minWidth: 900, minHeight: 600,
    title: 'Coyote Studio',
    backgroundColor: '#1e1e1e',
    webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: true, nodeIntegration: false, sandbox: false },
  });
  win.loadFile(path.join(__dirname, '..', 'renderer', 'index.html'));
  win.on('closed', () => { win = null; });
}

function buildMenu() {
  const t = (action) => () => send('menu', action);
  Menu.setApplicationMenu(Menu.buildFromTemplate([
    { label: '&File', submenu: [
      { label: 'New Project...', accelerator: 'CmdOrCtrl+Shift+N', click: t('newProject') },
      { label: 'New File', accelerator: 'CmdOrCtrl+N', click: t('newFile') },
      { label: 'Open Folder...', accelerator: 'CmdOrCtrl+K CmdOrCtrl+O', click: t('openFolder') },
      { label: 'Open File...', accelerator: 'CmdOrCtrl+O', click: t('openFile') },
      { type: 'separator' },
      { label: 'Save', accelerator: 'CmdOrCtrl+S', click: t('save') },
      { label: 'Save All', accelerator: 'CmdOrCtrl+Alt+S', click: t('saveAll') },
      { label: 'Close Editor', accelerator: 'CmdOrCtrl+W', click: t('closeTab') },
      { type: 'separator' },
      { role: 'quit' }] },
    { label: '&Edit', submenu: [{ role: 'undo' }, { role: 'redo' }, { type: 'separator' }, { role: 'cut' }, { role: 'copy' }, { role: 'paste' }, { role: 'selectAll' },
      { type: 'separator' }, { label: 'Format Document', accelerator: 'Shift+Alt+F', click: t('format') }] },
    { label: '&Run', submenu: [
      { label: 'Run File', accelerator: 'F5', click: t('run') },
      { label: 'Run Project (cpx start)', accelerator: 'Ctrl+F5', click: t('runProject') },
      { label: 'Start Debugging', accelerator: 'F6', click: t('debug') },
      { label: 'Stop', accelerator: 'Shift+F5', click: t('stop') },
      { type: 'separator' },
      { label: 'Continue', accelerator: 'F8', click: t('continue') },
      { label: 'Step Over', accelerator: 'F10', click: t('stepOver') },
      { label: 'Step Into', accelerator: 'F11', click: t('stepInto') },
      { label: 'Step Out', accelerator: 'Shift+F11', click: t('stepOut') },
      { label: 'Toggle Breakpoint', accelerator: 'F9', click: t('toggleBreakpoint') }] },
    { label: '&Tools', submenu: [
      { label: 'Check File', click: t('check') },
      { label: 'Show ML', click: t('showMl') },
      { label: 'Build Native Executable...', click: t('native') },
      { label: 'Build Library (.lib)', click: t('packLib') },
      { label: 'Build and Install Library for Me', click: t('installLib') },
      { label: 'Open My Libraries Folder', click: t('libFolder') },
      { label: 'Reload Libraries', click: t('reloadLibs') },
      { label: 'Install Packages (cpm install)', click: t('cpmInstall') },
      { label: 'Toolchain Doctor', click: t('doctor') },
      { label: 'Settings...', click: t('settings') }] },
    { label: '&View', submenu: [
      { label: 'Command Palette...', accelerator: 'CmdOrCtrl+Shift+P', click: t('palette') },
      { label: 'Go to File...', accelerator: 'CmdOrCtrl+P', click: t('quickOpen') },
      { label: 'Go to Symbol in Project...', accelerator: 'CmdOrCtrl+T', click: t('goSymbolWorkspace') },
      { type: 'separator' },
      { label: 'Explorer', accelerator: 'CmdOrCtrl+Shift+E', click: t('viewExplorer') },
      { label: 'Find in Files', accelerator: 'CmdOrCtrl+Shift+F', click: t('viewSearch') },
      { label: 'Run and Debug', accelerator: 'CmdOrCtrl+Shift+D', click: t('viewDebug') },
      { label: 'Libraries', accelerator: 'CmdOrCtrl+Shift+L', click: t('viewLibs') },
      { label: 'Toggle Sidebar', accelerator: 'CmdOrCtrl+B', click: t('toggleSidebar') },
      { label: 'Terminal', accelerator: 'CmdOrCtrl+`', click: t('terminal') },
      { label: 'Toggle Panel', accelerator: 'CmdOrCtrl+J', click: t('togglePanel') },
      { label: 'Toggle Dark / Light Theme', click: t('toggleTheme') },
      { type: 'separator' }, { role: 'reload' }, { role: 'toggleDevTools' }, { type: 'separator' }, { role: 'resetZoom' }, { role: 'zoomIn' }, { role: 'zoomOut' }, { role: 'togglefullscreen' }] },
    { label: '&Help', submenu: [
      { label: 'About Coyote Studio', click: t('about') },
      { label: 'Language reference (docs/language.md)', click: () => { const p = docPath('language.md'); if (p) shell.openPath(p); } }] },
  ]));
}

function docPath(name) {
  for (const base of [path.resolve(path.dirname(process.execPath), '..', 'docs'), path.resolve(__dirname, '..', '..', 'docs')]) {
    const p = path.join(base, name);
    if (fs.existsSync(p)) return p;
  }
  return null;
}

function handle(channel, fn) {
  ipcMain.handle(channel, async (_e, ...args) => {
    try { return await fn(...args); } catch (e) { return { ok: false, error: e.message || String(e) }; }
  });
}

function registerIpc() {
  handle('app:info', () => ({ version: VERSION, toolchain: tc, platform: process.platform, settings: settings.data, home: os.homedir(), docs: docPath('language.md') }));
  handle('app:setSetting', (k, v) => { settings.set(k, v); if (k === 'toolchainDir') refreshToolchain(); return { ok: true, toolchain: tc }; });
  handle('app:versions', () => actions.versions(tc));
  handle('app:doctor', () => actions.doctor(tc));

  handle('dialog:openFolder', async () => {
    const r = await dialog.showOpenDialog(win, { properties: ['openDirectory', 'createDirectory'] });
    return r.canceled ? null : r.filePaths[0];
  });
  handle('dialog:openFile', async () => {
    const r = await dialog.showOpenDialog(win, { properties: ['openFile'], filters: [{ name: 'Coyote', extensions: ['coy'] }, { name: 'All files', extensions: ['*'] }] });
    return r.canceled ? null : r.filePaths[0];
  });
  handle('dialog:saveAs', async (def) => {
    const r = await dialog.showSaveDialog(win, { defaultPath: def, filters: [{ name: 'Coyote', extensions: ['coy'] }] });
    return r.canceled ? null : r.filePath;
  });
  handle('dialog:saveExe', async (def) => {
    const r = await dialog.showSaveDialog(win, { defaultPath: def });
    return r.canceled ? null : r.filePath;
  });
  handle('dialog:confirm', async (message, detail, buttons) => {
    const r = await dialog.showMessageBox(win, { type: 'question', message, detail, buttons: buttons || ['OK', 'Cancel'], defaultId: 0, cancelId: (buttons || ['OK', 'Cancel']).length - 1 });
    return r.response;
  });

  handle('fs:list', (dir) => listDir(dir));
  handle('fs:read', (file) => ({ ok: true, text: fs.readFileSync(file, 'utf8') }));
  handle('fs:write', (file, text) => { fs.writeFileSync(file, text, 'utf8'); return { ok: true }; });
  handle('fs:create', (file, text) => {
    if (fs.existsSync(file)) return { ok: false, error: 'already exists' };
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, text || '', 'utf8');
    return { ok: true };
  });
  handle('fs:mkdir', (dir) => { fs.mkdirSync(dir, { recursive: true }); return { ok: true }; });
  handle('fs:rename', (a, b) => { if (fs.existsSync(b)) return { ok: false, error: 'target exists' }; fs.renameSync(a, b); return { ok: true }; });
  handle('fs:delete', (p) => { fs.rmSync(p, { recursive: true, force: true }); return { ok: true }; });
  handle('fs:reveal', (p) => { shell.showItemInFolder(p); return { ok: true }; });

  handle('project:open', (dir) => {
    settings.addRecent(dir);
    const root = findProjectRoot(dir);
    return { ok: true, root, manifest: root ? readManifest(root) : null };
  });
  handle('project:root', (file) => { const root = findProjectRoot(file); return { root, manifest: root ? readManifest(root) : null }; });
  handle('project:new', async (parent, name, kind) => {
    const dir = path.join(parent, name);
    if (fs.existsSync(dir) && fs.readdirSync(dir).length) return { ok: false, error: 'the folder already exists and is not empty' };
    fs.mkdirSync(dir, { recursive: true });
    const r = await require('./runner').capture(tc.tools.cpm, ['-C', dir, 'init', name, ...(kind === 'library' ? ['--library'] : [])], { cwd: dir });
    return { ok: r.code === 0, dir, error: (r.stderr || r.stdout || r.error || '').trim() };
  });

  handle('lib:pack', (root, install) => actions.packLibrary(tc, root, !!install));
  handle('tool:check', (file, cwd) => actions.check(tc, file, cwd));
  handle('tool:checkText', (file, text, cwd) => actions.checkText(tc, file, text, cwd));
  handle('tool:formatText', (text) => actions.formatText(tc, text));
  handle('lang:libs', (projectRoot) => ({ ok: true, ...actions.readLibs(tc, projectRoot) }));
  handle('fs:files', (root) => ({ ok: true, files: search.listFiles(root) }));
  handle('fs:search', (root, query, opts) => ({ ok: true, ...search.searchText(root, query, opts || {}) }));
  handle('shell:openPath', async (p) => { fs.mkdirSync(p, { recursive: true }); const e = await shell.openPath(p); return { ok: !e, error: e }; });
  handle('shell:openExternal', (url) => { if (/^https?:\/\//.test(url)) shell.openExternal(url); return { ok: true }; });
  handle('tool:format', (file, cwd) => actions.format(tc, file, cwd));
  handle('tool:showMl', (file, cwd) => actions.showMl(tc, file, cwd));
  handle('tool:native', (file, out, cwd) => actions.buildNative(tc, file, out, cwd));

  handle('run:file', (file, cwd) => {
    if (running) return { ok: false, error: 'something is already running' };
    running = actions.runFile(tc, file, { cwd, onOut: (p, t) => send('run:out', { phase: p, text: t }), onErr: (p, t) => send('run:err', { phase: p, text: t }) });
    const h = running;
    h.done.then((r) => { if (running === h) running = null; send('run:exit', r); });
    return { ok: true };
  });
  handle('run:workflow', (root, name) => {
    if (running) return { ok: false, error: 'something is already running' };
    const h = actions.runWorkflow(tc, root, name, { onOut: (t) => send('run:out', { phase: 'run', text: t }), onErr: (t) => send('run:err', { phase: 'run', text: t }) });
    running = h;
    h.done.then((r) => { if (running === h) running = null; send('run:exit', { code: r.code, phase: 'run', error: r.error }); });
    return { ok: true };
  });
  handle('run:cpm', (root, args) => {
    if (running) return { ok: false, error: 'something is already running' };
    const h = actions.cpm(tc, root, args, { onOut: (t) => send('run:out', { phase: 'run', text: t }), onErr: (t) => send('run:err', { phase: 'run', text: t }) });
    running = h;
    h.done.then((r) => { if (running === h) running = null; send('run:exit', { code: r.code, phase: 'run', error: r.error }); });
    return { ok: true };
  });
  handle('run:stdin', (text) => { if (running) running.write(text); else if (session) session.sendInput(text.replace(/\r?\n$/, '')); return { ok: true }; });
  handle('run:closeStdin', () => { if (running) running.closeStdin(); if (session) session.endInput(); return { ok: true }; });
  handle('run:stdinDebug', (text) => { if (session) session.sendInput(text); return { ok: true }; });
  handle('run:stop', () => { if (running) running.kill(); if (session) session.stop(); return { ok: true }; });

  handle('debug:start', async (file, cwd, breakpoints) => {
    if (running || session) return { ok: false, error: 'something is already running' };
    const s = new DebugSession(tc);
    session = s;
    s.on('output', (text) => send('debug:event', { event: 'output', text }));
    s.on('log', (text) => send('debug:event', { event: 'log', text }));
    s.on('stopped', (e) => send('debug:event', e));
    s.on('runtimeError', (e) => send('debug:event', e));
    s.on('inputRequested', () => send('debug:event', { event: 'inputRequested' }));
    s.on('exited', (e) => { if (session === s) session = null; send('debug:event', { event: 'exited', ...e }); });
    try {
      await s.start(file, { cwd, breakpoints });
    } catch (e) {
      session = null;
      return { ok: false, error: e.compileOutput || e.message, compile: true };
    }
    return { ok: true };
  });
  const dbg = (name, fn) => handle(name, async (...a) => (session ? { ok: true, result: await fn(session, ...a) } : { ok: false, error: 'no debug session' }));
  dbg('debug:break', (s, f, l) => s.setBreakpoint(f, l));
  dbg('debug:unbreak', (s, f, l) => s.clearBreakpoint(f, l));
  dbg('debug:continue', (s) => s.resume());
  dbg('debug:stepOver', (s) => s.stepOver());
  dbg('debug:stepInto', (s) => s.stepInto());
  dbg('debug:stepOut', (s) => s.stepOut());
  dbg('debug:stack', (s) => s.stack());
  dbg('debug:vars', (s, frame) => s.variables(frame));
  dbg('debug:globals', (s) => s.globals());
}

if (!app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on('second-instance', (_e, argv, cwd) => {
    const f = pathFromArgv(argv, cwd);
    if (win) { if (win.isMinimized()) win.restore(); win.focus(); if (f) send('open-path', f); }
  });
}

app.whenReady().then(() => {
  pendingOpen = pathFromArgv(process.argv, process.cwd());
  settings = new Settings(path.join(app.getPath('userData'), 'settings.json'));
  refreshToolchain();
  registerIpc();
  ipcMain.handle('app:takeOpenPath', () => { const f = pendingOpen; pendingOpen = null; return f; });
  buildMenu();
  createWindow();
  app.on('activate', () => { if (BrowserWindow.getAllWindows().length === 0) createWindow(); });
});

app.on('window-all-closed', () => {
  if (running) running.kill();
  if (session) session.stop();
  app.quit();
});

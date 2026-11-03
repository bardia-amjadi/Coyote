'use strict';
/* global studio */
// Project Manager (side view) and the project build commands: Build, Rebuild, Clean, Open output folder.
// What a build produces comes from the project file (<Name>.cyproj, docs/projects.md), never from buttons.

const projectInfo = () => (S.manifest && S.manifest.ok ? S.manifest : null);
const isLibraryProject = () => { const m = projectInfo(); return !!(m && m.manifest.type === 'library'); };
const outputPath = () => { const m = projectInfo(); return m && S.project && !isLibraryProject() ? join(S.project, m.settings.file) : null; };

function sourceFiles() {
  const root = S.project;
  if (!root) return [];
  const out = [];
  for (const e of S.ws.values()) {
    const rel = norm(e.path);
    if (norm(e.path).toLowerCase().startsWith(norm(root).toLowerCase() + '/') && rel.endsWith('.coy')) out.push(rel.slice(norm(root).length + 1));
  }
  return out.sort();
}

function lines(text) { return String(text || '').split(/\r?\n/).map((x) => x.trim()).filter(Boolean); }

function renderProjectView() {
  const info = projectInfo();
  $('pm-empty').classList.toggle('hidden', !!info);
  $('pm-body').classList.toggle('hidden', !info);
  if (!info) {
    $('pm-empty-text').textContent = !S.root ? 'No folder is open.' : S.manifest && S.manifest.error && S.manifest.file ? 'The project file has an error: ' + S.manifest.error : 'This folder has no project file (.cyproj).';
    $('pm-create').classList.toggle('hidden', !S.root || !!(S.manifest && S.manifest.file));
    return;
  }
  const m = info.manifest;
  const st = info.settings;
  $('pm-title').textContent = m.name || base(S.project);
  $('pm-file').textContent = base(info.file) + (info.kind === 'packages' ? ' (packages.json: same settings, older file name)' : '');
  $('pm-name').value = m.name || '';
  $('pm-version').value = m.version || '';
  $('pm-type').value = m.type === 'library' ? 'library' : 'app';
  $('pm-desc').value = m.description || '';
  const files = sourceFiles();
  const entry = info.entry;
  const opts = files.includes(entry) || !entry ? files : [entry, ...files];
  $('pm-entry').innerHTML = opts.map((f) => `<option value="${esc(f)}" ${f === entry ? 'selected' : ''}>${esc(f)}</option>`).join('');
  $('pm-outkind').value = st.kind;
  $('pm-outmode').value = st.mode;
  $('pm-outmode').disabled = st.kind === 'ml';
  $('pm-outdir').value = st.directory;
  $('pm-outname').value = m.output && m.output.name ? m.output.name : '';
  $('pm-outname').placeholder = m.name || '';
  $('pm-keep').checked = st.keepUnused;
  $('pm-args').value = st.args.join('\n');
  syncProjectType();
  $('pm-count').textContent = `(${files.length})`;
  const list = $('pm-files');
  list.innerHTML = '';
  for (const f of files) {
    const d = row(`<span class="ico coy">◆</span><b>${esc(f)}</b>${f === entry ? ' <span class="badge">entry</span>' : ''}`, f === entry ? 'entry' : '');
    d.onclick = () => App.openFile(join(S.project, f));
    d.oncontextmenu = (e) => { e.preventDefault(); App.ctxMenu(e.clientX, e.clientY, [{ label: 'Open', run: () => App.openFile(join(S.project, f)) }, ...(isLibraryProject() ? [] : [{ label: 'Set as entry point', run: () => setEntry(f) }])]); };
    list.append(d);
  }
  if (!files.length) list.innerHTML = '<div class="empty">no .coy files yet</div>';
}
function syncProjectType() {
  const lib = $('pm-type').value === 'library';
  for (const id of ['pm-out-title', 'pm-out-form', 'pm-run-title', 'pm-run-form']) $(id).classList.toggle('hidden', lib);
  $('pm-entry').disabled = lib;
  $('pm-open').textContent = lib ? 'Library folder' : 'Output folder';
  $('pm-outmode').disabled = $('pm-outkind').value === 'ml';
}

function collectProjectPatch() {
  const g = (id) => $(id).value.trim();
  const patch = { name: g('pm-name'), version: g('pm-version'), type: g('pm-type'), description: g('pm-desc') };
  if (patch.type === 'app') {
    patch.entry = g('pm-entry');
    patch.output = { directory: g('pm-outdir') || 'bin', kind: g('pm-outkind'), mode: g('pm-outmode'), keepUnused: $('pm-keep').checked };
    if (g('pm-outname')) patch.output.name = g('pm-outname');
    patch.run = { args: lines($('pm-args').value) };
  }
  return patch;
}
async function saveProjectProps(quiet) {
  if (!S.project) return false;
  const patch = collectProjectPatch();
  if (!patch.name) { App.toast('The project needs a name.', 'error'); return false; }
  const r = await studio.project.save(S.project, patch);
  if (r.ok === false) { App.toast('save project: ' + r.error, 'error'); return false; }
  S.manifest = r.manifest;
  if (!quiet) App.toast('Project saved', 'good');
  renderProjectView();
  App.updateProjectUi();
  return true;
}
async function setEntry(rel) {
  const r = await studio.project.save(S.project, { entry: rel });
  if (r.ok === false) return App.toast(r.error, 'error');
  S.manifest = r.manifest;
  renderProjectView();
  App.toast('Entry point: ' + rel, 'good');
}
async function createProjectFile() {
  if (!S.root) return;
  const name = await App.prompt('Create project file', 'Project name', base(S.root).replace(/[^A-Za-z0-9_.-]/g, '-'));
  if (!name) return;
  // the entry: the open .coy file inside the folder, else the first .coy file, else main.coy
  const rootN = norm(S.root).toLowerCase() + '/';
  const t = S.active && S.tabs.get(S.active);
  let entry = t && t.path && norm(t.path).toLowerCase().startsWith(rootN) && t.path.endsWith('.coy') ? norm(t.path).slice(rootN.length) : null;
  if (!entry) { const f = [...S.ws.values()].map((e) => norm(e.path)).filter((p) => p.toLowerCase().startsWith(rootN) && p.endsWith('.coy')).sort()[0]; entry = f ? f.slice(rootN.length) : 'main.coy'; }
  const r = await studio.project.createFile(S.root, name, entry);
  if (r.ok === false) return App.toast('create project: ' + r.error, 'error');
  await App.openFolder(S.root);
  App.showView('project');
}

// ---------- build ----------
async function cleanProject(quiet) {
  if (!S.project) return;
  const r = await studio.project.clean(S.project);
  if (!quiet) {
    App.showTab('output');
    App.outLine(r.removed && r.removed.length ? 'Clean: removed ' + r.removed.map((p) => norm(p).slice(norm(S.project).length + 1)).join(', ') : 'Clean: nothing to remove', 'info');
    App.toast('Clean done', 'good');
  }
  await App.renderTree();
}
async function buildProject(kind) {
  if (S.running) return App.toast('Something is already running. Stop it first.', 'error');
  if (!S.project || !projectInfo()) {
    App.showView('project');
    return App.toast('Build needs a project. Create one with File > New Project, or use "Create project file here" in the Project Manager.', 'error', 6000);
  }
  if ($('pm-body') && !$('pm-body').classList.contains('hidden') && dirtyProps()) { if (!(await saveProjectProps(true))) return; }
  await App.saveAll();
  if (kind === 'rebuild') await cleanProject(true);
  App.showTab('output');
  if (App.S.info.settings.clearOutputOnBuild !== false) App.outTerm.clear();
  const m = projectInfo();
  if (isLibraryProject()) {
    App.outLine(`> ${kind} library ${m.manifest.name}`, 'info');
    return App.packLib(false);
  }
  const st = m.settings;
  App.outLine(`> ${kind} ${m.manifest.name}  (${st.kind === 'ml' ? 'ML file' : 'executable, mode ' + st.mode}) -> ${st.file}`, 'info');
  S.runKind = 'build';
  App.setRunning(true, 'building', { build: true });
  const r = await studio.run.workflow(S.project, 'build');
  if (r.ok === false) { App.outLine(r.error, 'err'); S.runKind = 'run'; App.setRunning(false); }
}
function onBuildExit(r) {
  S.runKind = 'run';
  const ok = r.code === 0;
  const out = outputPath();
  if (ok) {
    App.outLine('Build succeeded' + (out ? ' -> ' + out : ''), 'good');
    App.toast('Build succeeded', 'good');
  } else {
    App.outLine(`Build failed (exit code ${r.code}${r.error ? ': ' + r.error : ''})`, 'err');
    App.toast('Build failed', 'error');
  }
  App.setRunning(false);
  App.renderTree();
}
async function openOutputFolder() {
  if (!S.project) return App.toast('No project is open.', 'error');
  const dir = isLibraryProject() ? join(S.project, 'out') : join(S.project, (projectInfo() && projectInfo().settings.directory) || 'bin');
  const r = await studio.shell.openPath(dir);
  if (r && r.ok === false) App.toast(r.error, 'error');
}

function propsSnapshot() { return JSON.stringify(collectProjectPatch()); }
let savedSnapshot = '';
function dirtyProps() { return !!projectInfo() && propsSnapshot() !== savedSnapshot; }
function snapshotProps() { savedSnapshot = projectInfo() ? propsSnapshot() : ''; }

function wireProjectView() {
  $('pm-build').onclick = () => buildProject('build');
  $('pm-rebuild').onclick = () => buildProject('rebuild');
  $('pm-clean').onclick = () => cleanProject(false);
  $('pm-run').onclick = () => App.runProject();
  $('pm-debug').onclick = () => App.startDebug();
  $('pm-open').onclick = openOutputFolder;
  $('pm-save').onclick = async () => { if (await saveProjectProps(false)) snapshotProps(); };
  $('pm-revert').onclick = () => { renderProjectView(); snapshotProps(); };
  $('pm-create').onclick = createProjectFile;
  $('pm-new').onclick = () => App.MENU.newProject();
  $('pm-type').onchange = syncProjectType;
  $('pm-outkind').onchange = syncProjectType;
}

Object.assign(App, { renderProjectView, wireProjectView, buildProject, cleanProject, onBuildExit, openOutputFolder, saveProjectProps, createProjectFile, isLibraryProject, outputPath, snapshotProps, setEntry });

'use strict';
/* global studio, CoyoteThemes */
// The Settings page: categories on the left, search on top, every change applies at once (and is saved).
(function () {
  const T = CoyoteThemes;
  const CATS = [['appearance', 'Appearance'], ['colors', 'Colors'], ['syntax', 'Syntax colors'], ['editor', 'Editor'], ['intellisense', 'Suggestions and checks'], ['run', 'Terminal and app window'], ['build', 'Build and run'], ['toolchain', 'Toolchain']];

  // generic rows: [key, label, type, options|{min,max}, description, defaultValue]
  const ROWS = {
    appearance: [
      ['uiFontSize', 'Interface font size', 'number', { min: 10, max: 20 }, 'Size of the text in menus, panels and views (pixels).', 13],
      ['density', 'Density', 'select', [['comfortable', 'Comfortable'], ['compact', 'Compact']], 'Compact reduces the spacing in lists and panels.', 'comfortable'],
    ],
    editor: [
      ['fontSize', 'Font size', 'number', { min: 8, max: 40 }, 'Editor font size (pixels).', 14],
      ['fontFamily', 'Font family', 'text', null, 'Empty uses Cascadia Mono / Consolas.', ''],
      ['fontLigatures', 'Font ligatures', 'check', null, 'Join characters such as => and == when the font supports it.', false],
      ['lineHeight', 'Line height', 'number', { min: 0, max: 60 }, 'In pixels. 0 = automatic.', 0],
      ['tabSize', 'Tab size', 'select', [['2', '2'], ['4', '4'], ['8', '8']], 'Spaces per indentation level.', 4],
      ['wordWrap', 'Word wrap', 'check', null, 'Wrap long lines instead of scrolling.', false],
      ['minimap', 'Minimap', 'check', null, 'Overview of the file at the right edge.', false],
      ['lineNumbers', 'Line numbers', 'select', [['on', 'On'], ['relative', 'Relative'], ['off', 'Off']], '', 'on'],
      ['renderWhitespace', 'Show whitespace', 'select', [['none', 'Never'], ['selection', 'In the selection'], ['boundary', 'Between words'], ['all', 'Always']], '', 'selection'],
      ['cursorStyle', 'Cursor style', 'select', [['line', 'Line'], ['block', 'Block'], ['underline', 'Underline']], '', 'line'],
      ['cursorBlinking', 'Cursor animation', 'select', [['blink', 'Blink'], ['smooth', 'Smooth'], ['phase', 'Phase'], ['expand', 'Expand'], ['solid', 'None']], '', 'blink'],
      ['smoothScrolling', 'Smooth scrolling', 'check', null, '', true],
      ['bracketPairs', 'Color matching brackets', 'check', null, 'Nested brackets get different colors.', true],
      ['indentGuides', 'Indent guides', 'check', null, 'Vertical lines for each indentation level.', true],
      ['stickyScroll', 'Sticky scroll', 'check', null, 'Keep the enclosing class or method visible at the top while scrolling.', true],
      ['formatOnSave', 'Format on save', 'check', null, '', false],
      ['autoSave', 'Auto save', 'select', [['off', 'Off'], ['afterDelay', 'After a short delay']], '', 'off'],
    ],
    intellisense: [
      ['checkOnType', 'Check for errors while typing', 'check', null, 'Runs the compiler on the open file and underlines problems.', true],
      ['quickSuggestions', 'Suggest while typing', 'check', null, 'Show the completion list as you type, without pressing Ctrl+Space.', true],
      ['acceptOnEnter', 'Accept a suggestion with Enter', 'select', [['on', 'Always'], ['smart', 'Only when it changes the text'], ['off', 'Never (Tab only)']], '', 'on'],
      ['parameterHints', 'Parameter hints', 'check', null, 'Show the parameters of a method while writing a call.', true],
      ['suggestImports', 'Suggest classes from libraries and namespaces not yet used', 'check', null, 'Picking one adds the matching use line.', true],
    ],
    run: [
      ['appWindow', 'Show the running program in its own window', 'select', [['debug', 'When debugging'], ['always', 'Run and debug'], ['never', 'Never (Terminal panel only)']], 'The window mirrors the Terminal panel and takes input.', 'debug'],
      ['terminalFontSize', 'Terminal font size', 'number', { min: 9, max: 24 }, 'Terminal, Output and Problems panels (pixels).', 13],
    ],
    build: [
      ['clearOutputOnBuild', 'Clear the Output panel before a build', 'check', null, 'What a build produces is set in the project file (Project Manager), not here.', true],
    ],
  };
  const get = (s, k, d) => (s[k] === undefined ? d : s[k]);

  let page = null, cur = 'appearance', filter = '';
  const q = (id) => page.querySelector('#' + id);
  async function set(k, v) {
    await studio.setSetting(k, v);
    App.S.info = await studio.info();
    App.applySettings();
  }
  const esc = (x) => App.esc(x);

  function rowHtml(s, [key, label, type, opt, desc, def]) {
    const v = get(s, key, def);
    let ctl = '';
    if (type === 'check') ctl = `<input type="checkbox" data-k="${key}" ${v ? 'checked' : ''}>`;
    else if (type === 'select') ctl = `<select data-k="${key}">${opt.map(([val, lab]) => `<option value="${val}" ${String(v) === val ? 'selected' : ''}>${lab}</option>`).join('')}</select>`;
    else if (type === 'number') ctl = `<input type="text" data-k="${key}" data-num="1" data-min="${opt.min}" data-max="${opt.max}" value="${v}" style="width:80px">`;
    else ctl = `<input type="text" data-k="${key}" value="${esc(v)}">`;
    return `<div class="srow" data-text="${esc((label + ' ' + desc).toLowerCase())}"><div class="slabel"><b>${esc(label)}</b>${desc ? `<span class="muted small">${esc(desc)}</span>` : ''}</div><div class="sctl">${ctl}<button class="mini srst" data-reset="${key}" title="Reset to default">Reset</button></div></div>`;
  }
  const colorRow = (k, label, value, isDefault, kind) => `<div class="srow" data-text="${esc(label.toLowerCase())}"><div class="slabel"><b>${esc(label)}</b></div><div class="sctl"><input type="color" data-${kind}="${k}" value="${value}"><code>${value}</code><button class="mini srst" data-reset-${kind}="${k}" ${isDefault ? 'disabled' : ''}>Reset</button></div></div>`;

  function renderCategory(s, info) {
    const body = q('set-body');
    let html = '';
    if (cur === 'appearance') {
      html += '<h4>Color theme</h4><div class="themes">' + Object.entries(T.PRESETS).map(([id, p]) => {
        const sw = ['bg', 'bg2', 'accent', 'editor', 'fg'].map((k) => `<i style="background:${p.vars[k]}"></i>`).join('');
        return `<div class="themecard ${s.theme === id ? 'sel' : ''}" data-theme="${id}"><div class="sw">${sw}</div><b>${esc(p.name)}</b></div>`;
      }).join('') + '</div>';
      const acc = T.isHex(s.accentColor) ? '#' + s.accentColor.replace('#', '') : T.resolve(s).vars.accent;
      html += colorRow('accentColor', 'Accent color (overrides the theme)', acc, !T.isHex(s.accentColor), 'accent');
    }
    if (cur === 'colors') {
      const r = T.resolve(s);
      html += '<p class="muted small">Change any color of the window. Colors you change are kept when you switch theme; Reset returns one to the theme\'s own value.</p>';
      html += T.VAR_LABELS.map(([k, label]) => colorRow(k, label, r.vars[k], !(s.themeColors && T.isHex(s.themeColors[k])), 'var')).join('');
      html += '<div style="margin-top:14px"><button id="reset-colors">Reset all colors</button></div>';
    }
    if (cur === 'syntax') {
      const r = T.resolve(s);
      html += '<p class="muted small">Colors of the code in the editor.</p>';
      html += T.SYNTAX_GROUPS.map(([id, label]) => colorRow(id, label, '#' + r.syntax[id], !(s.syntaxColors && T.isHex(s.syntaxColors[id])), 'syn')).join('');
      html += '<div style="margin-top:14px"><button id="reset-syntax">Reset all syntax colors</button></div>';
      html += `<pre class="synprev" style="background:${r.vars.editor}"><span style="color:#${r.syntax.comment}">// preview</span>\n<span style="color:#${r.syntax.keyword}">use</span> <span style="color:#${r.syntax.string}">"Standard"</span>\n<span style="color:#${r.syntax.keyword}">class</span> <span style="color:#${r.syntax.type}">Dog</span> <span style="color:#${r.syntax.operator}">:</span> <span style="color:#${r.syntax.type}">Animal</span> {\n    <span style="color:#${r.syntax.keyword}">override method</span> <span style="color:#${r.syntax.function}">speak</span>(<span style="color:#${r.syntax.type}">int</span> <span style="color:#${r.syntax.variable}">n</span>) {\n        <span style="color:#${r.syntax.control}">return</span> <span style="color:#${r.syntax.property}">name</span> <span style="color:#${r.syntax.operator}">+</span> <span style="color:#${r.syntax.number}">42</span>\n    }\n}</pre>`;
    }
    if (ROWS[cur]) html += ROWS[cur].map((r) => rowHtml(s, r)).join('');
    if (cur === 'toolchain') {
      html += `<div class="srow"><div class="slabel"><b>Toolchain folder</b><span class="muted small">Empty = the installed Coyote. Using: ${esc(info.toolchain.dir || 'not found')}</span></div><div class="sctl"><input type="text" data-k="toolchainDir" value="${esc(s.toolchainDir || '')}" style="width:320px"><button id="tc-browse">Browse...</button></div></div>
      <div class="srow"><div class="slabel"><b>Toolchain doctor</b><span class="muted small">Shows which tools were found, including clang for native builds.</span></div><div class="sctl"><button id="tc-doctor">Run doctor</button></div></div>
      <div style="margin-top:22px"><button id="reset-all">Reset ALL settings to defaults</button></div>`;
    }
    body.innerHTML = html;
    wire(s);
    applyFilter();
  }

  function applyFilter() {
    const f = filter.trim().toLowerCase();
    for (const r of page.querySelectorAll('.srow')) r.classList.toggle('hidden', !!f && !r.dataset.text.includes(f));
  }

  function wire(s) {
    const body = q('set-body');
    for (const c of body.querySelectorAll('.themecard')) c.onclick = async () => { await set('theme', c.dataset.theme); refresh(); };
    for (const el of body.querySelectorAll('[data-k]')) {
      const k = el.dataset.k;
      el.onchange = async () => {
        let v;
        if (el.type === 'checkbox') v = el.checked;
        else if (el.dataset.num) { v = Math.min(+el.dataset.max, Math.max(+el.dataset.min, parseInt(el.value, 10) || 0)); el.value = v; } else if (k === 'tabSize') v = parseInt(el.value, 10); else v = el.value.trim();
        if (k === 'toolchainDir') { const x = await studio.setSetting(k, v); App.S.info = await studio.info(); App.updateToolchainStatus(); await App.loadLibs(); if (!x.toolchain.dir) App.toast('The Coyote tools were not found in that folder.', 'error'); refresh(); return; }
        await set(k, v);
      };
    }
    for (const b of body.querySelectorAll('[data-reset]')) b.onclick = async () => { const d = App.S.info.defaults || {}; await set(b.dataset.reset, d[b.dataset.reset] === undefined ? '' : d[b.dataset.reset]); refresh(); };
    const mapSet = (name, attr) => {
      for (const el of body.querySelectorAll(`[data-${attr}]`)) {
        const k = el.dataset[attr];
        el.oninput = () => { el.nextElementSibling.textContent = el.value; };
        el.onchange = async () => {
          if (attr === 'accent') await set('accentColor', el.value);
          else { const m = { ...(App.S.info.settings[name] || {}) }; m[k] = el.value; await set(name, m); }
          refresh();
        };
      }
      for (const b of body.querySelectorAll(`[data-reset-${attr}]`)) b.onclick = async () => {
        if (attr === 'accent') await set('accentColor', '');
        else { const m = { ...(App.S.info.settings[name] || {}) }; delete m[b.dataset['reset' + attr[0].toUpperCase() + attr.slice(1)]]; await set(name, m); }
        refresh();
      };
    };
    mapSet('themeColors', 'var'); mapSet('syntaxColors', 'syn'); mapSet('', 'accent');
    const on = (id, fn) => { const e = q(id); if (e) e.onclick = fn; };
    on('reset-colors', async () => { await set('themeColors', {}); await set('accentColor', ''); refresh(); });
    on('reset-syntax', async () => { await set('syntaxColors', {}); refresh(); });
    on('tc-browse', async () => { const d = await studio.dialog.openFolder(); if (d) { await studio.setSetting('toolchainDir', d); App.S.info = await studio.info(); App.updateToolchainStatus(); await App.loadLibs(); refresh(); } });
    on('tc-doctor', () => { close(); App.MENU.doctor(); });
    on('reset-all', async () => {
      if ((await studio.dialog.confirm('Reset every setting to its default?')) !== 0) return;
      const d = App.S.info.defaults || {};
      for (const k of Object.keys(d)) if (!['recent', 'lastProjectDir', 'toolchainDir'].includes(k)) await studio.setSetting(k, d[k]);
      App.S.info = await studio.info(); App.applySettings(); refresh();
    });
  }

  async function refresh() {
    const info = await studio.info();
    App.S.info = info;
    for (const b of page.querySelectorAll('#set-nav button')) b.classList.toggle('active', b.dataset.cat === cur);
    renderCategory(info.settings, info);
  }

  async function open(cat) {
    if (cat) cur = cat;
    if (!page) {
      page = document.createElement('div');
      page.id = 'settings-page';
      page.innerHTML = `<div id="set-head"><b>Settings</b><input id="set-search" type="text" placeholder="Search settings (this category)"><span class="grow"></span><button id="set-close" class="primary">Close</button></div>
        <div id="set-main"><div id="set-nav">${CATS.map(([id, label]) => `<button data-cat="${id}">${label}</button>`).join('')}</div><div id="set-body"></div></div>`;
      document.body.append(page);
      q('set-close').onclick = close;
      q('set-search').oninput = (e) => { filter = e.target.value; applyFilter(); };
      for (const b of page.querySelectorAll('#set-nav button')) b.onclick = () => { cur = b.dataset.cat; filter = ''; q('set-search').value = ''; refresh(); };
      page.addEventListener('keydown', (e) => { if (e.key === 'Escape') close(); });
    }
    page.classList.remove('hidden');
    await refresh();
    q('set-search').focus();
  }
  function close() { if (page) page.classList.add('hidden'); if (App.editor) App.editor.focus(); }

  Object.assign(App, { openSettings: open, closeSettings: close });
})();

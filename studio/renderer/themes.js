'use strict';
// Theme engine: presets, user color overrides and syntax colors. Everything is driven by settings:
//   theme (preset id), accentColor, themeColors { var: '#rrggbb' }, syntaxColors { group: 'rrggbb' },
//   uiFontSize, density, terminalFontSize.
// The page colors are CSS variables on <body>; the editor colors are a Monaco theme ("coyote-live") built from the same data.
(function () {
  const V = (o) => o;
  const SYNTAX_GROUPS = [
    ['comment', 'Comments', ['comment']],
    ['keyword', 'Keywords (method, class, use ...)', ['keyword.declaration', 'keyword.modifier', 'keyword', 'constant.language']],
    ['control', 'Control flow (if, for, return ...)', ['keyword.control']],
    ['type', 'Types and classes', ['type', 'type.identifier', 'class', 'class.defaultLibrary', 'interface', 'enum', 'namespace']],
    ['function', 'Methods and functions', ['entity.name.function', 'function', 'method']],
    ['variable', 'Variables and parameters', ['variable', 'variable.name', 'parameter']],
    ['property', 'Properties and fields', ['variable.property', 'property']],
    ['string', 'Strings and characters', ['string', 'string.char', 'string.lib']],
    ['escape', 'String escapes', ['string.escape']],
    ['number', 'Numbers', ['number', 'number.float']],
    ['operator', 'Operators and punctuation', ['operator', 'delimiter']],
    ['invalid', 'Invalid text', ['invalid']],
  ];
  const VAR_LABELS = [
    ['bg', 'Window background'], ['bg2', 'Side panel'], ['bg3', 'Toolbar / title'], ['bg4', 'Raised elements'], ['fg', 'Text'], ['muted', 'Secondary text'],
    ['border', 'Borders'], ['accent', 'Accent'], ['sel', 'Selected row'], ['hover', 'Hover row'], ['input', 'Input fields'],
    ['err', 'Error'], ['ok', 'Success'], ['warn', 'Warning'], ['link', 'Links'], ['status', 'Status bar'], ['status-debug', 'Status bar (debugging)'], ['editor', 'Editor background'],
  ];

  const PRESETS = {
    dark: { name: 'Coyote Dark', base: 'dark', vars: V({ bg: '#1f1f1f', bg2: '#252526', bg3: '#2d2d30', bg4: '#333337', fg: '#d4d4d4', muted: '#8b8b8b', border: '#3a3a3d', accent: '#7a5cff', 'accent-fg': '#ffffff', sel: '#37373d', hover: '#2a2d33', input: '#3c3c3c', err: '#f48771', ok: '#6ec07a', warn: '#e5c07b', link: '#6cb6ff', status: '#5a3fd0', 'status-debug': '#c4561d', editor: '#1e1e1e' }),
      syntax: { comment: '6a9955', keyword: '569cd6', control: 'c586c0', type: '4ec9b0', function: 'dcdcaa', variable: '9cdcfe', property: '9cdcfe', string: 'ce9178', escape: 'd7ba7d', number: 'b5cea8', operator: 'd4d4d4', invalid: 'f44747' } },
    light: { name: 'Coyote Light', base: 'light', vars: V({ bg: '#f3f3f3', bg2: '#f0f0f0', bg3: '#e6e6e6', bg4: '#dcdcdc', fg: '#2b2b2b', muted: '#6b6b6b', border: '#cfcfcf', accent: '#5b3fd6', 'accent-fg': '#ffffff', sel: '#dde4f2', hover: '#e4e8f0', input: '#ffffff', err: '#c72e2e', ok: '#2b8a3e', warn: '#9a6700', link: '#0b63c5', status: '#5b3fd6', 'status-debug': '#c4561d', editor: '#ffffff' }),
      syntax: { comment: '008000', keyword: '0000ff', control: 'af00db', type: '267f99', function: '795e26', variable: '001080', property: '001080', string: 'a31515', escape: 'ee0000', number: '098658', operator: '000000', invalid: 'cd3131' } },
    midnight: { name: 'Midnight', base: 'dark', vars: V({ bg: '#0d1117', bg2: '#10161f', bg3: '#161b22', bg4: '#1c232d', fg: '#c9d1d9', muted: '#7d8590', border: '#232a35', accent: '#58a6ff', 'accent-fg': '#04101f', sel: '#1b2a3d', hover: '#151d29', input: '#161b22', err: '#ff7b72', ok: '#56d364', warn: '#e3b341', link: '#79c0ff', status: '#1f4f8f', 'status-debug': '#b4531c', editor: '#0d1117' }),
      syntax: { comment: '6e7681', keyword: 'ff7b72', control: 'ff7b72', type: 'ffa657', function: 'd2a8ff', variable: 'c9d1d9', property: '79c0ff', string: 'a5d6ff', escape: '79c0ff', number: '79c0ff', operator: 'ff7b72', invalid: 'ffa198' } },
    graphite: { name: 'Graphite', base: 'dark', vars: V({ bg: '#262626', bg2: '#2b2b2b', bg3: '#313131', bg4: '#3a3a3a', fg: '#dcdcdc', muted: '#929292', border: '#404040', accent: '#e0a458', 'accent-fg': '#1c1405', sel: '#3d3d3d', hover: '#333333', input: '#383838', err: '#ee7b6b', ok: '#8fbf7f', warn: '#e0c070', link: '#8ab4d8', status: '#7a5a22', 'status-debug': '#b34d1b', editor: '#222222' }),
      syntax: { comment: '7f8c7a', keyword: 'e0a458', control: 'd98c8c', type: '8fc0b0', function: 'e6d79a', variable: 'd8d8d8', property: 'a9c4dd', string: 'b7d08f', escape: 'e0a458', number: 'd9a6c8', operator: 'c8c8c8', invalid: 'f07070' } },
    solarized: { name: 'Solarized Dark', base: 'dark', vars: V({ bg: '#002b36', bg2: '#073642', bg3: '#0a3c49', bg4: '#12495a', fg: '#93a1a1', muted: '#657b83', border: '#14505f', accent: '#268bd2', 'accent-fg': '#ffffff', sel: '#0f4655', hover: '#0b3d4a', input: '#0a3c49', err: '#dc322f', ok: '#859900', warn: '#b58900', link: '#2aa198', status: '#1b6a99', 'status-debug': '#cb4b16', editor: '#002b36' }),
      syntax: { comment: '586e75', keyword: '859900', control: 'cb4b16', type: 'b58900', function: '268bd2', variable: '93a1a1', property: '2aa198', string: '2aa198', escape: 'cb4b16', number: 'd33682', operator: '93a1a1', invalid: 'dc322f' } },
    contrast: { name: 'High contrast', base: 'dark', vars: V({ bg: '#000000', bg2: '#000000', bg3: '#0b0b0b', bg4: '#1a1a1a', fg: '#ffffff', muted: '#bdbdbd', border: '#6fc3df', accent: '#ffd400', 'accent-fg': '#000000', sel: '#26266b', hover: '#1a1a1a', input: '#000000', err: '#ff6b6b', ok: '#4cff7a', warn: '#ffd400', link: '#6fc3ff', status: '#003a66', 'status-debug': '#b34000', editor: '#000000' }),
      syntax: { comment: '7ca668', keyword: '569cd6', control: 'c586c0', type: '4ec9b0', function: 'dcdcaa', variable: '9cdcfe', property: '9cdcfe', string: 'ce9178', escape: 'd7ba7d', number: 'b5cea8', operator: 'ffffff', invalid: 'ff0000' } },
  };

  const hex = (c) => String(c || '').replace('#', '');
  const isHex = (c) => /^#?[0-9a-fA-F]{6}$/.test(String(c || ''));

  // The effective values for the current settings: preset, then the user's overrides.
  function resolve(s) {
    const p = PRESETS[s.theme] || PRESETS.dark;
    const vars = { ...p.vars };
    const tc = s.themeColors || {};
    for (const k of Object.keys(vars)) if (isHex(tc[k])) vars[k] = '#' + hex(tc[k]);
    if (isHex(s.accentColor)) vars.accent = '#' + hex(s.accentColor);
    const syntax = { ...p.syntax };
    const sc = s.syntaxColors || {};
    for (const k of Object.keys(syntax)) if (isHex(sc[k])) syntax[k] = hex(sc[k]);
    return { preset: p, vars, syntax, base: p.base };
  }

  // black or white text, whichever reads better on the given color
  function readableOn(c) {
    const n = parseInt(hex(c), 16);
    const r = (n >> 16) & 255, g = (n >> 8) & 255, b = n & 255;
    return (0.299 * r + 0.587 * g + 0.114 * b) > 150 ? '#000000' : '#ffffff';
  }

  function monacoThemeData(r) {
    const rules = [];
    for (const [id, , tokens] of SYNTAX_GROUPS) for (const t of tokens) rules.push({ token: t, foreground: r.syntax[id] });
    const dark = r.base === 'dark';
    const ed = r.vars.editor;
    return { base: r.preset === PRESETS.contrast ? 'hc-black' : dark ? 'vs-dark' : 'vs', inherit: true, rules, colors: {
      'editor.background': ed, 'editor.foreground': r.vars.fg, 'editorLineNumber.foreground': r.vars.muted, 'editorLineNumber.activeForeground': r.vars.fg,
      'editor.lineHighlightBackground': r.vars.hover + (r.preset === PRESETS.contrast ? '' : '99'), 'editor.selectionBackground': r.vars.accent + '55', 'editorCursor.foreground': r.vars.accent,
      'editorWidget.background': r.vars.bg2, 'editorWidget.border': r.vars.border, 'editorSuggestWidget.background': r.vars.bg2, 'editorSuggestWidget.border': r.vars.border, 'editorSuggestWidget.selectedBackground': r.vars.sel,
      'editorHoverWidget.background': r.vars.bg2, 'editorHoverWidget.border': r.vars.border, 'editorIndentGuide.background1': r.vars.border, 'input.background': r.vars.input,
    } };
  }

  function applyAppearance(s) {
    const r = resolve(s);
    const body = document.body;
    body.classList.toggle('theme-light', r.base === 'light');
    body.classList.toggle('theme-dark', r.base !== 'light');
    for (const [k, v] of Object.entries(r.vars)) body.style.setProperty('--' + k, v);
    body.style.setProperty('--accent-fg', readableOn(r.vars.accent));
    body.style.fontSize = (Math.min(20, Math.max(10, parseInt(s.uiFontSize, 10) || 13))) + 'px';
    body.style.setProperty('--term-size', (Math.min(24, Math.max(9, parseInt(s.terminalFontSize, 10) || 13))) + 'px');
    body.classList.toggle('density-compact', s.density === 'compact');
    if (window.App && App.monaco) {
      App.monaco.editor.defineTheme('coyote-live', monacoThemeData(r));
      App.monaco.editor.setTheme('coyote-live');
    }
    return r;
  }

  window.CoyoteThemes = { PRESETS, SYNTAX_GROUPS, VAR_LABELS, resolve, applyAppearance, readableOn, isHex, monacoThemeData };
})();

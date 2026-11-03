'use strict';
/* global CoyoteLang */
// Registers the Coyote language in Monaco: tokenizer, themes, semantic colors and the language service
// (completion with automatic `use`/`import`, hover, signature help, go to definition, outline, quick fixes, formatting).
(function () {
  const L = CoyoteLang;
  const CONTROL = ['if', 'else', 'while', 'for', 'break', 'continue', 'return'];
  const DECL = ['hand', 'method', 'class', 'model', 'interface', 'property', 'constructor', 'import', 'export', 'use', 'var', 'new', 'in', 'is', 'as', 'this', 'base', 'get', 'set'];
  const MOD = ['public', 'private', 'protected', 'static', 'override'];
  const KW_SET = new Set(L.KEYWORDS.filter((k) => k !== 'get' && k !== 'set'));
  const TYPE_SET = new Set(L.TYPES);

  const HOST_BUILTINS = [
    { module: 'con', name: 'print', params: [{ type: 'string', name: 'text' }], ret: '', doc: 'Writes text without a line break.' },
    { module: 'con', name: 'println', params: [{ type: 'string', name: 'text' }], ret: '', doc: 'Writes text and a line break.' },
    { module: 'con', name: 'readLine', params: [], ret: 'string', doc: 'Reads one line typed by the user (in Studio: in the Terminal panel).' },
    { module: 'con', name: 'hasInput', params: [], ret: 'boolean', doc: 'true when more input is available.' },
  ];

  function install(monaco, host) {
    const K = monaco.languages.CompletionItemKind;
    monaco.languages.register({ id: 'coyote', extensions: ['.coy'], aliases: ['Coyote'] });
    monaco.languages.setLanguageConfiguration('coyote', {
      comments: { lineComment: '#' },
      brackets: [['{', '}'], ['[', ']'], ['(', ')']],
      autoClosingPairs: [{ open: '{', close: '}' }, { open: '[', close: ']' }, { open: '(', close: ')' }, { open: '"', close: '"', notIn: ['string'] }, { open: "'", close: "'", notIn: ['string', 'comment'] }],
      surroundingPairs: [{ open: '{', close: '}' }, { open: '(', close: ')' }, { open: '[', close: ']' }, { open: '"', close: '"' }, { open: "'", close: "'" }],
      indentationRules: { increaseIndentPattern: /\{[^}"']*$/, decreaseIndentPattern: /^\s*\}/ },
      onEnterRules: [{ beforeText: /^\s*#.*$/, action: { indentAction: monaco.languages.IndentAction.None } }],
      wordPattern: /[A-Za-z_]\w*|\d+(?:\.\d+)?/,
      folding: { offSide: false, markers: { start: /^\s*#region\b/, end: /^\s*#endregion\b/ } },
    });

    monaco.languages.setMonarchTokensProvider('coyote', {
      controlKw: CONTROL, declKw: DECL, modKw: MOD, types: L.TYPES.filter((t) => t !== 'Guid'), constants: L.LITERALS,
      tokenizer: {
        root: [
          [/#.*$/, 'comment'],
          [/\b(use)(\s+)(")/, ['keyword.declaration', '', { token: 'string', next: '@libname' }]],
          [/\b(import)(\s+)(")/, ['keyword.declaration', '', { token: 'string', next: '@string' }]],
          [/\b(hand)(\s*)(:)(\s*)([A-Za-z_]\w*)(\s+)([A-Za-z_]\w*)/, ['keyword.declaration', '', 'delimiter', '', 'namespace', '', 'entity.name.function']],
          [/\b(method)(\s+)([A-Za-z_]\w*)/, ['keyword.declaration', '', 'entity.name.function']],
          [/\b(class|model|interface)(\s+)([A-Za-z_]\w*)/, ['keyword.declaration', '', { token: 'type.identifier', next: '@header' }]],
          [/\b(new)(\s+)([A-Za-z_]\w*)/, ['keyword.declaration', '', { cases: { '@types': 'type', '@default': 'type.identifier' } }]],
          [/\b(property)(\s+)([A-Za-z_]\w*(?:<[^>()]*>)?\??)(\s+)([A-Za-z_]\w*)/, ['keyword.declaration', '', 'type', '', 'variable.name']],
          [/"/, 'string', '@string'],
          [/'(?:[^'\\]|\\.)'/, 'string.char'],
          [/\b\d+\.\d+(?:[eE][+-]?\d+)?\b/, 'number.float'],
          [/\b\d+\b/, 'number'],
          [/(\.)(\s*)([A-Za-z_]\w*)(?=\s*\()/, ['delimiter', '', 'entity.name.function']],
          [/(\.)(\s*)([A-Za-z_]\w*)/, ['delimiter', '', 'variable.property']],
          [/\bGuid\b/, 'type'],
          [/[A-Z][A-Za-z0-9_]*(?=\s*\.)/, 'type.identifier'],
          [/[A-Z][A-Za-z0-9_]*(?=\??\s+[A-Za-z_])/, 'type.identifier'],
          [/[A-Za-z_]\w*(?=\s*\()/, { cases: { '@controlKw': 'keyword.control', '@declKw': 'keyword.declaration', '@types': 'type', '@default': 'entity.name.function' } }],
          [/[A-Za-z_]\w*/, { cases: { '@controlKw': 'keyword.control', '@declKw': 'keyword.declaration', '@modKw': 'keyword.modifier', '@types': 'type', '@constants': 'constant.language', '@default': 'variable' } }],
          [/\?\?|[=!<>]=|&&|\|\||[+\-*\/%]=?|[<>!=?]/, 'operator'],
          [/[{}()\[\]]/, '@brackets'],
          [/[,;:]/, 'delimiter'],
        ],
        header: [[/[ \t]+/, ''], [/[:,<>]/, 'delimiter'], [/\?/, 'operator'], [/[A-Za-z_]\w*/, { cases: { '@types': 'type', '@default': 'type.identifier' } }], [/\{/, { token: '@brackets', next: '@pop' }], [/$/, '', '@pop'], [/./, '', '@pop']],
        string: [[/[^\\"]+/, 'string'], [/\\[\\"ntr0']/, 'string.escape'], [/\\./, 'invalid'], [/"/, 'string', '@pop']],
        libname: [[/[^"]+/, 'string.lib'], [/"/, 'string', '@pop']],
      },
    });

    const dark = [
      ['comment', '6a9955'], ['keyword.declaration', '569cd6'], ['keyword.modifier', '569cd6'], ['keyword.control', 'c586c0'], ['keyword', '569cd6'],
      ['type', '4ec9b0'], ['type.identifier', '4ec9b0'], ['class', '4ec9b0'], ['class.defaultLibrary', '4ec9b0'], ['namespace', '4ec9b0'],
      ['entity.name.function', 'dcdcaa'], ['function', 'dcdcaa'], ['method', 'dcdcaa'], ['variable.property', '9cdcfe'], ['property', '9cdcfe'], ['variable.name', '9cdcfe'],
      ['variable', '9cdcfe'], ['parameter', '9cdcfe'], ['number', 'b5cea8'], ['number.float', 'b5cea8'], ['string', 'ce9178'], ['string.char', 'ce9178'],
      ['string.lib', 'd7ba7d'], ['string.escape', 'd7ba7d'], ['constant.language', '569cd6'], ['operator', 'd4d4d4'], ['delimiter', 'd4d4d4'], ['invalid', 'f44747'],
    ];
    const light = [
      ['comment', '008000'], ['keyword.declaration', '0000ff'], ['keyword.modifier', '0000ff'], ['keyword.control', 'af00db'], ['keyword', '0000ff'],
      ['type', '267f99'], ['type.identifier', '267f99'], ['class', '267f99'], ['class.defaultLibrary', '267f99'], ['namespace', '267f99'],
      ['entity.name.function', '795e26'], ['function', '795e26'], ['method', '795e26'], ['variable.property', '001080'], ['property', '001080'], ['variable.name', '001080'],
      ['variable', '001080'], ['parameter', '001080'], ['number', '098658'], ['number.float', '098658'], ['string', 'a31515'], ['string.char', 'a31515'],
      ['string.lib', '795e26'], ['string.escape', 'ee0000'], ['constant.language', '0000ff'], ['operator', '000000'], ['delimiter', '000000'], ['invalid', 'cd3131'],
    ];
    const toRules = (a) => a.map(([token, foreground]) => ({ token, foreground }));
    monaco.editor.defineTheme('coyote-dark', { base: 'vs-dark', inherit: true, rules: toRules(dark), colors: { 'editor.background': '#1e1e1e', 'editorLineNumber.foreground': '#6e7681', 'editor.lineHighlightBackground': '#2a2d3340' } });
    monaco.editor.defineTheme('coyote-light', { base: 'vs', inherit: true, rules: toRules(light), colors: { 'editor.background': '#ffffff' } });

    // ------------------------------------------------------------ analysis cache
    const cache = new Map();
    function ctxFor(model) {
      const key = model.uri.toString();
      const c = cache.get(key);
      const gen = host.generation();
      if (c && c.version === model.getVersionId() && c.gen === gen) return c.ctx;
      const imported = host.importedSymbols(model);
      const ctx = L.analyze(model.getValue(), { libs: host.libs(), workspaceClasses: imported.classes, workspaceMethods: imported.methods });
      cache.set(key, { version: model.getVersionId(), gen, ctx });
      return ctx;
    }
    monaco.editor.onWillDisposeModel((m) => cache.delete(m.uri.toString()));

    // every exported library symbol, for completion of names that have not been `use`d yet
    function libSymbols() {
      const out = [];
      if (host.settings && host.settings().suggestImports === false) return out;
      for (const lib of host.libs().values()) {
        for (const c of lib.classes) out.push({ sym: c, lib: lib.name });
        for (const m of lib.methods) out.push({ sym: m, lib: lib.name });
      }
      return out;
    }

    // ------------------------------------------------------------ helpers
    const md = (sym, extra) => {
      const parts = ['```coyote\n' + L.signatureOf(sym) + '\n```'];
      if (sym.doc) parts.push(sym.doc);
      if (extra) parts.push(extra);
      return { value: parts.join('\n\n') };
    };
    const kindOf = (s) => ({ method: K.Method, ctor: K.Constructor, property: K.Property, field: K.Field, class: K.Class, model: K.Struct, var: K.Variable, hand: K.Function }[s.kind] ?? K.Field);
    const methodSnippet = (name, params) => {
      if (!params || !params.length) return name + '()';
      return name + '(' + params.map((p, i) => '${' + (i + 1) + ':' + (p.name || 'arg' + (i + 1)).replace(/[$}\\]/g, '') + '}').join(', ') + ')';
    };
    const rangeAt = (model, pos) => { const w = model.getWordUntilPosition(pos); return new monaco.Range(pos.lineNumber, w.startColumn, pos.lineNumber, w.endColumn); };
    const useEdit = (model, lib) => {
      const ins = L.useInsertion(model.getValue(), lib);
      return [{ range: new monaco.Range(ins.line, 1, ins.line, 1), text: ins.text }];
    };
    const importEdit = (model, spec) => {
      const text = model.getValue();
      const lines = text.split(/\r?\n/);
      let last = 0;
      for (let i = 0; i < lines.length; i++) if (/^\s*(use|import)\s+"/.test(lines[i])) last = i + 1;
      const at = last ? last + 1 : L.useInsertion(text, 'x').line;
      return [{ range: new monaco.Range(at, 1, at, 1), text: `import "${spec}"\n` + (!last && lines[at - 1] && lines[at - 1].trim() ? '\n' : '') }];
    };

    // ------------------------------------------------------------ completion
    monaco.languages.registerCompletionItemProvider('coyote', {
      triggerCharacters: ['.', '"', ':', ' '],
      provideCompletionItems(model, pos) {
        try { return completeAt(model, pos); } catch (e) { console.error('completion failed: ' + (e && e.stack || e)); return { suggestions: [] }; }
      },
    });
    function completeAt(model, pos) {
      {
        const ctx = ctxFor(model);
        const line = model.getLineContent(pos.lineNumber);
        const prefix = line.slice(0, pos.column - 1);
        const range = rangeAt(model, pos);
        const items = [];
        let m;

        // use "<library>"
        if ((m = /^\s*use\s+"([^"]*)$/.exec(prefix))) {
          const r = new monaco.Range(pos.lineNumber, pos.column - m[1].length, pos.lineNumber, pos.column);
          for (const lib of host.libs().values()) {
            items.push({ label: lib.name, kind: K.Module, insertText: lib.name, range: r, detail: 'library ' + lib.version, sortText: lib.name.toLowerCase(),
              documentation: { value: (lib.requires.length ? 'Includes: ' + lib.requires.join(', ') + '\n\n' : '') + 'Classes: ' + (lib.classes.map((c) => c.name).join(', ') || '(none)') } });
          }
          return { suggestions: items };
        }
        // hand:module name(...)
        if ((m = /\bhand\s*:\s*(\w*)$/.exec(prefix))) {
          const r = new monaco.Range(pos.lineNumber, pos.column - m[1].length, pos.lineNumber, pos.column);
          const mods = new Map();
          for (const h of HOST_BUILTINS) mods.set(h.module, (mods.get(h.module) || 0) + 1);
          for (const lib of host.libs().values()) for (const h of lib.hands) mods.set(h.module, (mods.get(h.module) || 0) + 1);
          for (const [name, n] of mods) items.push({ label: name, kind: K.Module, insertText: name, range: r, detail: n + ' host function(s)' });
          return { suggestions: items };
        }
        if ((m = /\bhand\s*:\s*(\w+)\s+(\w*)$/.exec(prefix))) {
          const r = new monaco.Range(pos.lineNumber, pos.column - m[2].length, pos.lineNumber, pos.column);
          const seen = new Set();
          const all = [...HOST_BUILTINS];
          for (const lib of host.libs().values()) for (const h of lib.hands) all.push(h);
          for (const h of all) {
            if (h.module !== m[1]) continue;
            const sig = `${h.name}(${L.paramsText(h.params)})${h.ret ? ': ' + h.ret : ''}`;
            if (seen.has(sig)) continue;
            seen.add(sig);
            items.push({ label: h.name, kind: K.Function, insertText: sig, range: r, detail: sig, documentation: h.doc || '' });
          }
          return { suggestions: items };
        }

        // import "<file of this project>"
        if ((m = /^\s*import\s+"([^"]*)$/.exec(prefix))) {
          const r = new monaco.Range(pos.lineNumber, pos.column - m[1].length, pos.lineNumber, pos.column);
          for (const f of host.importFiles(model)) items.push({ label: f.spec, kind: K.File, insertText: f.spec, range: r, detail: f.exports.length ? 'exports: ' + f.exports.join(', ') : 'no exported symbols', sortText: f.spec.toLowerCase() });
          return { suggestions: items };
        }
        const enc = L.localsAt(ctx, pos.lineNumber);
        // class Name<T> : <base class, interfaces>
        if ((m = /^\s*(?:(?:public|private|export)\s+)*(class|interface)\s+\w+\s*(?:<[^>]*>)?\s*:\s*(?:[^{]*,\s*)?(\w*)$/.exec(prefix))) {
          const r = new monaco.Range(pos.lineNumber, pos.column - m[2].length, pos.lineNumber, pos.column);
          const self = (/class|interface\s+(\w+)/.exec(prefix) || [])[1];
          const used = new Set((prefix.split(':')[1] || '').split(',').map((x) => x.trim().replace(/<.*$/, '')));
          for (const [name, c] of ctx.classes) {
            if (c.kind === 'model' || name === self || used.has(name)) continue;
            if (m[1] === 'interface' && c.kind !== 'interface') continue;
            items.push({ label: name, kind: c.kind === 'interface' ? K.Interface : K.Class, insertText: name, range: r, detail: c.kind + (c.supers && c.supers.length ? ' : ' + c.supers.join(', ') : ''), documentation: md(c), sortText: (c.kind === 'interface' ? '1' : '0') + name });
          }
          for (const { sym, lib } of libSymbols()) {
            if (ctx.classes.has(sym.name) || (sym.kind !== 'class' && sym.kind !== 'interface') || L.hasUse(model.getValue(), lib) || (m[1] === 'interface' && sym.kind !== 'interface')) continue;
            items.push({ label: { label: sym.name, description: lib }, kind: sym.kind === 'interface' ? K.Interface : K.Class, insertText: sym.name, range: r, detail: `${lib} - adds: use "${lib}"`, sortText: '3' + sym.name, additionalTextEdits: useEdit(model, lib) });
          }
          return { suggestions: items };
        }
        // override <cursor>: the methods of the base types that can be overridden, as complete declarations
        if (enc.cls && (m = /^\s*(?:(?:public|protected)\s+)*override\s+(?:method\s+)?(\w*)$/.exec(prefix))) {
          const r = new monaco.Range(pos.lineNumber, pos.column - m[1].length, pos.lineNumber, pos.column);
          const hasMethodKw = /\bmethod\s+\w*$/.test(prefix);
          for (const mem of L.overridable(ctx, enc.cls)) {
            const ps = L.paramsText(mem.params);
            items.push({ label: mem.name, kind: K.Method, range: r, detail: `override ${mem.container}.${mem.name}`, documentation: md(mem), sortText: '0' + mem.name,
              insertText: (hasMethodKw ? '' : 'method ') + `${mem.name}(${ps})${mem.ret ? ': ' + mem.ret : ''} {\n\t${mem.ret ? 'return $0' : '$0'}\n}`, insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet });
          }
          return { suggestions: items };
        }
        // base.<cursor>
        if (enc.cls && (m = /\bbase\s*\.\s*(\w*)$/.exec(prefix))) {
          for (const mem of L.baseMembers(ctx, enc.cls)) {
            const isM = mem.kind === 'method';
            items.push({ label: mem.name, kind: kindOf(mem), insertText: isM ? methodSnippet(mem.name, mem.params) : mem.name, insertTextRules: isM ? monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet : undefined, range, detail: L.signatureOf(mem), documentation: md(mem), sortText: (isM ? '1' : '0') + mem.name });
          }
          return { suggestions: items };
        }
        // x as <Type> / x is <Type> / new <Type>: class names (and built-in types)
        if ((m = /(?:\bas|\bis|\bnew)\s+(\w*)$/.exec(prefix)) && !/\bnew\s+array|\bnew\s+map/.test(prefix)) {
          const isNew = /\bnew\s+\w*$/.test(prefix);
          for (const [name, c] of ctx.classes) {
            if (isNew && c.kind === 'interface') continue;
            items.push({ label: name, kind: c.kind === 'interface' ? K.Interface : c.kind === 'model' ? K.Struct : K.Class, insertText: (c.typeParams && c.typeParams.length && isNew) ? `${name}<\${1:${c.typeParams.join(', ')}}>($0)` : name,
              insertTextRules: (c.typeParams && c.typeParams.length && isNew) ? monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet : undefined, range, detail: c.kind + (c.typeParams && c.typeParams.length ? '<' + c.typeParams.join(', ') + '>' : ''), documentation: md(c), sortText: '0' + name });
          }
          if (isNew) for (const k of ['array', 'map']) items.push({ label: k, kind: K.TypeParameter, insertText: k + '<', range, sortText: '1' + k });
          else for (const k of L.TYPES) items.push({ label: k, kind: K.TypeParameter, insertText: k, range, sortText: '2' + k });
          if (isNew || /\b(as|is)\s+\w*$/.test(prefix)) {
            for (const { sym, lib } of libSymbols()) {
              if (ctx.classes.has(sym.name) || (sym.kind !== 'class' && sym.kind !== 'model') || L.hasUse(model.getValue(), lib)) continue;
              items.push({ label: { label: sym.name, description: lib }, kind: K.Class, insertText: sym.name, range, detail: `${lib} - adds: use "${lib}"`, sortText: '3' + sym.name, additionalTextEdits: useEdit(model, lib) });
            }
          }
          return { suggestions: items };
        }

        // member access: expr.<cursor>
        const mm = /([A-Za-z_]\w*(?:\([^()]*\))?(?:\[[^\]]*\])*(?:\.[A-Za-z_]\w*(?:\([^()]*\))?(?:\[[^\]]*\])*)*)\s*\.\s*(\w*)$/.exec(prefix);
        if (mm) {
          const t = L.typeOfExpr(ctx, mm[1], pos.lineNumber);
          let members = [];
          let extraEdits = null;
          let from = '';
          if (t && t.staticClass) members = L.staticMembersOf(ctx, t.staticClass);
          else if (t && t.type) members = L.membersOf(ctx, t.type);
          else if (/^[A-Za-z_]\w*$/.test(mm[1])) {
            // a library class that has not been `use`d yet: offer its static members and add the `use` on accept
            const found = libSymbols().find((x) => x.sym.name === mm[1] && (x.sym.kind === 'class' || x.sym.kind === 'model'));
            if (found && !L.hasUse(model.getValue(), found.lib)) {
              members = found.sym.members.filter((x) => x.static && !x.private);
              extraEdits = useEdit(model, found.lib);
              from = found.lib;
            }
          }
          for (const mem of members) {
            if (mem.kind === 'ctor' || (mem.private && !(ctx.ix.classes.some((c) => c.name === mem.container)))) continue;
            const isM = mem.kind === 'method';
            items.push({
              label: mem.name, kind: kindOf(mem), insertText: isM ? methodSnippet(mem.name, mem.params) : mem.name,
              insertTextRules: isM ? monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet : undefined,
              range, detail: L.signatureOf(mem).replace(/^static /, ''), documentation: md(mem, from ? `Needs \`use "${from}"\` (added automatically).` : ''),
              additionalTextEdits: extraEdits || undefined, sortText: (mem.kind === 'method' ? '1' : '0') + mem.name,
            });
          }
          return { suggestions: items };
        }

        // plain names
        const here = L.localsAt(ctx, pos.lineNumber);
        const add = (label, kind, extra) => items.push({ label, kind, insertText: label, range, ...extra });
        for (const v of here.vars.values()) add(v.name, K.Variable, { detail: L.signatureOf({ ...v, type: L.normalizeVarType(ctx, v, pos.lineNumber) }), sortText: '0' + v.name });
        if (here.cls) for (const mem of here.cls.members) if (mem.kind !== 'ctor') add(mem.name, kindOf(mem), { detail: L.signatureOf(mem), documentation: md(mem), sortText: '1' + mem.name, insertText: mem.kind === 'method' ? methodSnippet(mem.name, mem.params) : mem.name, insertTextRules: mem.kind === 'method' ? monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet : undefined });
        for (const h of ctx.ix.hands) add(h.name, K.Function, { detail: L.signatureOf(h), documentation: md(h), sortText: '1' + h.name, insertText: methodSnippet(h.name, h.params), insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet });
        const seenNames = new Set(items.map((i) => i.label));
        for (const [name, sym] of ctx.methods) {
          if (sym.extension) continue;
          add(name, K.Function, { detail: L.signatureOf(sym), documentation: md(sym, sym.lib ? 'Library ' + sym.lib : ''), sortText: '2' + name, insertText: methodSnippet(name, sym.params), insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet });
        }
        for (const [name, sym] of ctx.classes) add(name, sym.kind === 'model' ? K.Struct : K.Class, { detail: sym.lib ? sym.lib + ' (in use)' : 'class', documentation: md(sym, sym.lib ? 'Library ' + sym.lib : ''), sortText: '2' + name, commitCharacters: ['.'] });
        // symbols from libraries that are not `use`d yet: choosing one adds the `use` line
        const have = new Set([...ctx.classes.keys(), ...ctx.methods.keys()]);
        for (const { sym, lib } of libSymbols()) {
          if (have.has(sym.name) || L.hasUse(model.getValue(), lib)) continue;
          const isClass = sym.kind === 'class' || sym.kind === 'model';
          items.push({
            label: { label: sym.name, description: lib }, kind: isClass ? K.Class : K.Function, insertText: isClass ? sym.name : methodSnippet(sym.name, sym.params),
            insertTextRules: isClass ? undefined : monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, filterText: sym.name, range,
            detail: `${lib} - adds: use "${lib}"`, documentation: md(sym, `Choosing this adds \`use "${lib}"\` to the top of the file.`), sortText: '3' + sym.name,
            additionalTextEdits: useEdit(model, lib), commitCharacters: isClass ? ['.'] : undefined,
          });
        }
        // exported symbols of other project files: choosing one adds an import
        for (const cand of host.importCandidates(model)) {
          if (seenNames.has(cand.sym.name) || have.has(cand.sym.name)) continue;
          const isClass = cand.sym.kind === 'class' || cand.sym.kind === 'model';
          items.push({ label: { label: cand.sym.name, description: cand.spec }, kind: isClass ? K.Class : K.Function, insertText: isClass ? cand.sym.name : methodSnippet(cand.sym.name, cand.sym.params),
            insertTextRules: isClass ? undefined : monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, filterText: cand.sym.name, range, detail: `adds: import "${cand.spec}"`, documentation: md(cand.sym), sortText: '3' + cand.sym.name,
            additionalTextEdits: importEdit(model, cand.spec) });
        }
        for (const k of L.KEYWORDS) if (k !== 'get' && k !== 'set') add(k, K.Keyword, { sortText: '5' + k, documentation: L.KEYWORD_DOCS[k] ? { value: L.KEYWORD_DOCS[k] } : undefined });
        for (const k of L.TYPES) add(k, K.TypeParameter, { sortText: '4' + k, documentation: L.TYPE_DOCS[k] ? { value: L.TYPE_DOCS[k] } : undefined });
        for (const k of L.LITERALS) add(k, K.Constant, { sortText: '5' + k });
        const snip = (label, text, doc) => items.push({ label, kind: K.Snippet, insertText: text, insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, range, documentation: doc, sortText: '6' + label, detail: 'snippet' });
        snip('method', 'method ${1:name}(${2}): ${3:int} {\n\t$0\n}', 'Method');
        snip('class', 'class ${1:Name} {\n\t$0\n}', 'Class');
        snip('classinherit', 'class ${1:Name} : ${2:Base} {\n\tconstructor(${3}) : base(${4}) { }\n\t$0\n}', 'Class that extends a base class');
        snip('interface', 'interface ${1:Name} {\n\tmethod ${2:Do}(): ${3:int}\n}', 'Interface: signatures only');
        snip('generic class', 'class ${1:Box}<${2:T}> {\n\t${2:T} value\n\tconstructor(${2:T} v) { value = v }\n\t$0\n}', 'Generic class');
        snip('generic method', 'method ${1:First}<${2:T}>(array<${2:T}> xs): ${2:T} {\n\treturn xs[0]\n}', 'Generic method (call it as First<int>(...))');
        snip('override', 'override method ${1:name}(${2}): ${3:int} {\n\t$0\n}', 'Override an inherited method');
        snip('model', 'model ${1:Name} {\n\t${2:int} ${3:field}\n}', 'Model: a class with a generated constructor');
        snip('for', 'for (int ${1:i} = 0; ${1:i} < ${2:n}; ${1:i} += 1) {\n\t$0\n}', 'Counting loop');
        snip('foreach', 'for (${1:var} ${2:item} in ${3:items}) {\n\t$0\n}', 'For-each loop');
        snip('if', 'if (${1:condition}) {\n\t$0\n}', 'If');
        snip('ifelse', 'if (${1:condition}) {\n\t$2\n} else {\n\t$0\n}', 'If / else');
        snip('while', 'while (${1:condition}) {\n\t$0\n}', 'While loop');
        snip('main', 'use "Standard"\n\n$0', 'Start a program with the whole Standard library');
        return { suggestions: items };
      }
    }

    // ------------------------------------------------------------ resolving a symbol under the cursor
    function symbolAt(model, pos) {
      const w = model.getWordAtPosition(pos);
      if (!w) return null;
      const ctx = ctxFor(model);
      const line = model.getLineContent(pos.lineNumber);
      const before = line.slice(0, w.startColumn - 1);
      const after = line.slice(w.endColumn - 1);
      const name = w.word;
      const range = new monaco.Range(pos.lineNumber, w.startColumn, pos.lineNumber, w.endColumn);
      const callLike = /^\s*\(/.test(after);
      let m;
      if ((m = /([A-Za-z_]\w*(?:\([^()]*\))?(?:\[[^\]]*\])*(?:\.[A-Za-z_]\w*(?:\([^()]*\))?(?:\[[^\]]*\])*)*)\s*\.\s*$/.exec(before))) {
        const t = L.typeOfExpr(ctx, m[1], pos.lineNumber);
        let mem = null;
        if (t && t.staticClass) mem = L.staticMembersOf(ctx, t.staticClass).find((x) => x.name === name);
        else if (t && t.type) mem = L.membersOf(ctx, t.type).find((x) => x.name === name);
        else {
          const found = libSymbols().find((x) => x.sym.name === m[1]);
          if (found) { const sm = found.sym.members && found.sym.members.find((x) => x.name === name && x.static); if (sm) return { sym: sm, range, lib: found.lib, unused: !L.hasUse(model.getValue(), found.lib) }; }
        }
        return mem ? { sym: mem, range, lib: mem.lib } : null;
      }
      if (/\buse\s+"[^"]*$/.test(before) || (/\buse\s+"$/.test(before))) {
        const lib = host.libs().get(name.toLowerCase());
        if (lib) return { libInfo: lib, range };
      }
      const here = L.localsAt(ctx, pos.lineNumber);
      if (here.vars.has(name) && !callLike) { const v = here.vars.get(name); return { sym: { ...v, type: L.normalizeVarType(ctx, v, pos.lineNumber) }, range }; }
      if (here.cls) { const mem = here.cls.members.find((x) => x.name === name && x.kind !== 'ctor'); if (mem) return { sym: mem, range }; }
      if (callLike && ctx.methods.has(name)) return { sym: ctx.methods.get(name), range, lib: ctx.methods.get(name).lib };
      const hand = ctx.ix.hands.find((h) => h.name === name);
      if (hand) return { sym: hand, range };
      if (ctx.classes.has(name)) return { sym: ctx.classes.get(name), range, lib: ctx.classes.get(name).lib };
      if (ctx.methods.has(name)) return { sym: ctx.methods.get(name), range, lib: ctx.methods.get(name).lib };
      const found = libSymbols().find((x) => x.sym.name === name);
      if (found) return { sym: found.sym, range, lib: found.lib, unused: true };
      if (L.KEYWORD_DOCS[name]) return { word: name, doc: L.KEYWORD_DOCS[name], range };
      if (L.TYPE_DOCS[name]) return { word: name, doc: L.TYPE_DOCS[name], range };
      return null;
    }

    monaco.languages.registerHoverProvider('coyote', {
      provideHover(model, pos) {
        const r = symbolAt(model, pos);
        if (!r) return null;
        if (r.libInfo) {
          const l = r.libInfo;
          return { range: r.range, contents: [{ value: '```coyote\nlibrary ' + l.name + ' ' + l.version + '\n```' }, { value: (l.requires.length ? 'Includes: ' + l.requires.join(', ') + '\n\n' : '') + 'Classes: ' + (l.classes.map((c) => c.name).join(', ') || '(none)') }] };
        }
        if (r.word) return { range: r.range, contents: [{ value: '```coyote\n' + r.word + '\n```' }, { value: r.doc }] };
        const extra = r.lib ? `*Library ${r.lib}*` + (r.unused ? ` - not used yet; accept it from the completion list or use the quick fix to add \`use "${r.lib}"\`.` : '') : '';
        return { range: r.range, contents: [md(r.sym, extra).value && { value: '```coyote\n' + L.signatureOf(r.sym) + '\n```' }, ...(r.sym.doc ? [{ value: r.sym.doc }] : []), ...(extra ? [{ value: extra }] : [])] };
      },
    });

    monaco.languages.registerSignatureHelpProvider('coyote', {
      signatureHelpTriggerCharacters: ['(', ','],
      provideSignatureHelp(model, pos) {
        const line = model.getLineContent(pos.lineNumber).slice(0, pos.column - 1);
        const call = L.activeCall(line);
        if (!call) return null;
        const ctx = ctxFor(model);
        let sym = null;
        const isNew = new RegExp('\\bnew\\s+' + call.callee.replace(/\./g, '\\.') + '\\s*$').test(line.slice(0, call.open) + call.callee) && /\bnew\s+[\w.]*$/.test(line.slice(0, call.open));
        const dot = call.callee.lastIndexOf('.');
        if (isNew) { const c = ctx.classes.get(call.callee); sym = c && c.members.find((x) => x.kind === 'ctor'); } else if (dot > 0) {
          const t = L.typeOfExpr(ctx, call.callee.slice(0, dot), pos.lineNumber);
          const name = call.callee.slice(dot + 1);
          if (t && t.staticClass) sym = L.staticMembersOf(ctx, t.staticClass).find((x) => x.name === name);
          else if (t && t.type) sym = L.membersOf(ctx, t.type).find((x) => x.name === name);
          else { const f = libSymbols().find((x) => x.sym.name === call.callee.slice(0, dot)); sym = f && f.sym.members && f.sym.members.find((x) => x.name === name && x.static); }
        } else {
          sym = ctx.ix.hands.find((h) => h.name === call.callee) || ctx.methods.get(call.callee);
          const here = L.localsAt(ctx, pos.lineNumber);
          if (!sym && here.cls) sym = here.cls.members.find((x) => x.name === call.callee && x.kind === 'method');
          if (!sym) { const f = libSymbols().find((x) => x.sym.name === call.callee); sym = f && f.sym; }
        }
        if (!sym || !sym.params) return null;
        const label = L.signatureOf(sym);
        const params = sym.params.map((p) => ({ label: p.name ? p.type + ' ' + p.name : p.type }));
        return { value: { signatures: [{ label, documentation: sym.doc ? { value: sym.doc } : undefined, parameters: params }], activeSignature: 0, activeParameter: Math.min(call.argIndex, Math.max(0, params.length - 1)) }, dispose() {} };
      },
    });

    // ------------------------------------------------------------ definitions and outline
    monaco.languages.registerDefinitionProvider('coyote', {
      provideDefinition(model, pos) {
        const r = symbolAt(model, pos);
        if (!r || !r.sym || !r.sym.line) return null;
        let uri = null;
        if (r.sym.lib && r.sym.file) uri = host.ensureLibModel(r.sym.lib, r.sym.file);
        else if (r.sym.kind === 'var' || (r.sym.container === undefined && !r.sym.lib && ctxFor(model).ix.hands.includes(r.sym)) || ctxFor(model).ix.classes.some((c) => c === r.sym || c.members.includes(r.sym)) || ctxFor(model).ix.methods.includes(r.sym) || ctxFor(model).ix.hands.includes(r.sym)) uri = model.uri;
        else if (r.sym.path) uri = host.ensureFileModel(r.sym.path);
        if (!uri) return null;
        const line = r.sym.line;
        return { uri, range: new monaco.Range(line, 1, line, 1) };
      },
    });

    monaco.languages.registerDocumentSymbolProvider('coyote', {
      provideDocumentSymbols(model) {
        const ix = ctxFor(model).ix;
        const SK = monaco.languages.SymbolKind;
        const rng = (a, b) => new monaco.Range(a, 1, Math.min(b, model.getLineCount()), model.getLineMaxColumn(Math.min(b, model.getLineCount())));
        const out = [];
        for (const c of ix.classes) {
          out.push({ name: c.name, detail: c.kind, kind: c.kind === 'model' ? SK.Struct : SK.Class, range: rng(c.line, c.endLine), selectionRange: rng(c.line, c.line), tags: [],
            children: c.members.map((m) => ({ name: m.kind === 'ctor' ? 'constructor' : m.name, detail: L.signatureOf(m), kind: { method: SK.Method, ctor: SK.Constructor, property: SK.Property, field: SK.Field }[m.kind] || SK.Field, range: rng(m.line, m.endLine || m.line), selectionRange: rng(m.line, m.line), tags: [] })) });
        }
        for (const m of ix.methods) out.push({ name: m.name, detail: L.signatureOf(m), kind: SK.Function, range: rng(m.line, m.endLine), selectionRange: rng(m.line, m.line), tags: [] });
        for (const h of ix.hands) out.push({ name: h.name, detail: L.signatureOf(h), kind: SK.Interface, range: rng(h.line, h.line), selectionRange: rng(h.line, h.line), tags: [] });
        return out;
      },
    });

    // ------------------------------------------------------------ quick fixes
    monaco.languages.registerCodeActionProvider('coyote', {
      provideCodeActions(model, range, context) {
        const actions = [];
        const ctx = ctxFor(model);
        for (const mk of context.markers) {
          const m = /^unknown (?:variable|class|function) '([A-Za-z_]\w*)'/.exec(mk.message);
          if (!m) continue;
          const name = m[1];
          const hits = libSymbols().filter((x) => x.sym.name === name && !L.hasUse(model.getValue(), x.lib));
          for (const h of hits) {
            actions.push({ title: `Add use "${h.lib}"`, kind: 'quickfix', diagnostics: [mk], isPreferred: hits.length === 1,
              edit: { edits: useEdit(model, h.lib).map((e) => ({ resource: model.uri, textEdit: e, versionId: undefined })) } });
          }
          for (const cand of host.importCandidates(model)) {
            if (cand.sym.name !== name) continue;
            actions.push({ title: `Add import "${cand.spec}"`, kind: 'quickfix', diagnostics: [mk], edit: { edits: importEdit(model, cand.spec).map((e) => ({ resource: model.uri, textEdit: e, versionId: undefined })) } });
          }
          if (!hits.length && ctx.visible.size === 0 && libSymbols().some((x) => x.sym.name === name) === false && host.libs().has('standard')) {
            actions.push({ title: 'Add use "Standard" (all standard libraries)', kind: 'quickfix', diagnostics: [mk], edit: { edits: useEdit(model, 'Standard').map((e) => ({ resource: model.uri, textEdit: e, versionId: undefined })) } });
          }
        }
        return { actions, dispose() {} };
      },
    }, { providedCodeActionKinds: ['quickfix'] });

    // ------------------------------------------------------------ semantic highlighting
    const TT = ['class', 'function', 'method', 'property', 'parameter', 'variable', 'namespace'];
    const TM = ['declaration', 'static', 'defaultLibrary'];
    monaco.languages.registerDocumentSemanticTokensProvider('coyote', {
      getLegend: () => ({ tokenTypes: TT, tokenModifiers: TM }),
      provideDocumentSemanticTokens(model) {
        const ctx = ctxFor(model);
        const data = [];
        let prevLine = 0, prevChar = 0;
        const total = model.getLineCount();
        for (let ln = 1; ln <= total; ln++) {
          const code = L.scanLine(model.getLineContent(ln)).code;
          const re = /[A-Za-z_]\w*/g;
          let m;
          let scope = null;
          while ((m = re.exec(code))) {
            const name = m[0];
            const start = m.index;
            if (KW_SET.has(name) || TYPE_SET.has(name) || name === 'true' || name === 'false' || name === 'null') continue;
            if (/^\d/.test(code[start - 1] || '')) continue;
            const before = code.slice(0, start);
            const after = code.slice(start + name.length);
            const prevCh = before.trimEnd().slice(-1);
            const prevWord = (/([A-Za-z_]\w*)\s*$/.exec(before) || [])[1];
            const callLike = /^\s*\(/.test(after);
            let type = -1, mods = 0;
            if (prevCh === '.') { type = callLike ? 2 : 3; } else if (prevCh === ':' && /\bhand\s*:\s*$/.test(before)) type = 6;
            else if (/\bhand\s*:\s*\w+\s+$/.test(before)) { type = 1; mods = 1; } else if (prevWord === 'method' || prevWord === 'constructor') { type = 1; mods = 1; } else if (prevWord === 'class' || prevWord === 'interface' || prevWord === 'model') { type = 0; mods = 1; } else if (prevWord === 'new') type = 0;
            else if (prevWord === 'property') type = -1;
            else if (callLike && !/^[A-Z]/.test(name) || (callLike && (ctx.methods.has(name)))) type = 1;
            else if (ctx.classes.has(name)) { type = 0; if (ctx.classes.get(name).lib) mods = 4; } else {
              if (!scope) scope = L.localsAt(ctx, ln);
              if (scope.vars.has(name)) type = scope.vars.get(name).param ? 4 : 5;
              else if (scope.cls && L.allMembers(ctx, scope.cls).some((x) => x.name === name && x.kind !== 'ctor' && x.kind !== 'method')) type = 3;
              else if (/^[A-Z]/.test(name) && /^\??\s+[A-Za-z_]/.test(after)) type = 0;
              else if (/^[A-Z]/.test(name) && /^\s*\./.test(after)) type = 0;
            }
            if (type < 0) continue;
            data.push(ln - 1 - prevLine, ln - 1 === prevLine ? start - prevChar : start, name.length, type, mods);
            prevLine = ln - 1;
            prevChar = start;
          }
        }
        return { data: new Uint32Array(data) };
      },
      releaseDocumentSemanticTokens() {},
    });

    // ------------------------------------------------------------ formatting (cdt fmt)
    monaco.languages.registerDocumentFormattingEditProvider('coyote', {
      async provideDocumentFormattingEdits(model) {
        const r = await host.formatText(model.getValue());
        if (!r || r.ok === false || typeof r.text !== 'string') return [];
        return [{ range: model.getFullModelRange(), text: r.text }];
      },
    });
  }

  window.registerCoyoteLanguage = install;
})();

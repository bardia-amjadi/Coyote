'use strict';
// Coyote language knowledge shared by the editor (renderer) and the unit tests (Node): reads .lib bundles and
// source text into symbol tables, resolves which libraries a file can see, and finds where a missing `use` goes.
// No DOM, no Monaco, no Node APIs: plain functions over strings.
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.CoyoteLang = factory();
})(typeof self !== 'undefined' ? self : this, function () {
  const KEYWORDS = ['if', 'else', 'while', 'for', 'in', 'is', 'break', 'continue', 'return', 'var', 'new', 'method', 'hand', 'class', 'model',
    'property', 'constructor', 'public', 'private', 'protected', 'static', 'override', 'interface', 'base', 'as', 'this', 'namespace', 'export', 'use', 'get', 'set'];
  const TYPES = ['int', 'double', 'boolean', 'string', 'char', 'Guid', 'obj', 'dyn', 'array', 'map', 'void'];
  const LITERALS = ['true', 'false', 'null'];
  const MODIFIERS = new Set(['public', 'private', 'protected', 'static', 'override', 'export']);

  const KEYWORD_DOCS = {
    method: 'Declares a method: `method name(int a, string b): int { ... }`. The return type is optional (no value).',
    hand: 'Declares a host function: `hand:module name(type param): returnType`. Usually you get these from the Standard libraries.',
    use: 'Brings in a namespace of this project, an installed package or a library: `use "MyApp.Models"`, `use "Standard.IO"`, or a library file `use "./My.lib"`.',
    namespace: 'Puts every declaration of the file into a namespace; it must be the first line: `namespace MyApp.Models`. Other files see its exports with `use "MyApp.Models"` (files of the same namespace see each other without it).',
    export: 'Makes a class, model or method visible to files that `use` its namespace (or library).',
    class: 'Declares a class with fields, properties, a constructor and methods.',
    model: 'Declares a class whose constructor takes every field in order.',
    var: 'Declares a variable whose type is taken from its initializer.',
    new: 'Creates an object (`new Counter(1)`), an array (`new array<int>(3)`) or a map (`new map<string,int>()`).',
    static: 'Member that belongs to the class itself, not to an instance.',
    property: 'A property: `property int Count` (auto) or `property int Doubled { get { return 2 * count } }`.',
    constructor: 'The initializer of a class: `constructor(int a) { ... }`.',
    this: 'The current object inside an instance method.',
    in: 'Used by `for (var x in items)`.',
    is: 'Type test: `if (v is string)` on an `obj`/`dyn` value, or `a is Dog` on a class value (true for Dog and its subclasses).',
    interface: 'Declares an interface: signatures only. `interface Shape { method Area(): double }`. A class implements it with `class Circle : Shape`.',
    protected: 'Member visible in its own class and in subclasses (not outside).',
    override: 'Marks a method that replaces an inherited one. Optional, but checked: it must match the base method exactly.',
    base: '`base(args)` in a constructor header calls the base constructor; `base.Method()` calls the base version of a method.',
    as: '`x as Dog`: converts a class value. An upcast always works; a downcast fails at run time with `invalid cast`.',
  };
  const TYPE_DOCS = {
    int: '32-bit signed integer. Overflow is a runtime error.', double: '64-bit floating point number.', boolean: '`true` or `false`.',
    string: 'Immutable text.', char: 'One Unicode code point: `\'a\'`.', Guid: 'A UUID.', obj: 'Holds a value of any type.',
    dyn: 'Like `obj`, but operators and members are checked at run time.', array: '`array<T>`: dynamic array. `.length`, `.push(x)`, `.copy()`.',
    map: '`map<K,V>`: insertion-ordered map.', void: 'No value.',
  };

  // ---------------------------------------------------------------- .lib bundles
  // Format (docs/libraries.md): "coyote-lib 1", "name X", "version V", optional "requires L" lines, then "file <path> <bytes>" + content.
  function parseLib(text) {
    const lib = { name: '', version: '', requires: [], files: [], error: null };
    let pos = 0;
    const nextLine = () => {
      if (pos >= text.length) return null;
      let i = text.indexOf('\n', pos);
      if (i < 0) i = text.length;
      const l = text.slice(pos, i).replace(/\r$/, '');
      pos = i + 1;
      return l;
    };
    if (nextLine() !== 'coyote-lib 1') { lib.error = 'not a coyote-lib 1 file'; return lib; }
    for (;;) {
      const save = pos;
      const l = nextLine();
      if (l === null) break;
      let m;
      if ((m = /^name\s+(.+)$/.exec(l))) lib.name = m[1].trim();
      else if ((m = /^version\s+(.+)$/.exec(l))) lib.version = m[1].trim();
      else if ((m = /^requires\s+(.+)$/.exec(l))) lib.requires.push(m[1].trim());
      else if ((m = /^file\s+(\S+)\s+(\d+)$/.exec(l))) {
        // The byte count is in UTF-8 bytes; the text is a JS string, so walk by bytes.
        const want = parseInt(m[2], 10);
        let end = pos, bytes = 0;
        while (end < text.length && bytes < want) {
          const c = text.codePointAt(end);
          bytes += c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
          end += c >= 0x10000 ? 2 : 1;
        }
        lib.files.push({ name: m[1], content: text.slice(pos, end) });
        pos = end;
        if (text[pos] === '\n') pos++;  // separator line break after the content
      } else if (l.trim() === '') continue;
      else { pos = save; break; }
    }
    if (!lib.name) lib.error = 'the library has no name';
    return lib;
  }

  // ---------------------------------------------------------------- source scanning
  // Splits a line into code (strings and comments blanked out, same length) and comment text.
  function scanLine(line) {
    let code = '';
    let i = 0;
    let comment = null;
    while (i < line.length) {
      const c = line[i];
      if (c === '#') { comment = line.slice(i + 1); code += ' '.repeat(line.length - i); break; }
      if (c === '"') {
        let j = i + 1;
        while (j < line.length && line[j] !== '"') j += line[j] === '\\' ? 2 : 1;
        const end = Math.min(j, line.length - 1);
        code += '"' + ' '.repeat(Math.max(0, end - i - 1)) + (j < line.length ? '"' : '');
        i = j + 1;
        continue;
      }
      if (c === "'" && /^'(?:[^'\\]|\\.)'/.test(line.slice(i))) {
        const m = /^'(?:[^'\\]|\\.)'/.exec(line.slice(i))[0];
        code += "'" + ' '.repeat(m.length - 2) + "'";
        i += m.length;
        continue;
      }
      code += c;
      i++;
    }
    return { code, comment };
  }

  function splitTop(s, sep) {
    const out = [];
    let depth = 0, cur = '';
    for (const c of s) {
      if (c === '<' || c === '(' || c === '[') depth++;
      else if (c === '>' || c === ')' || c === ']') depth--;
      if (c === sep && depth === 0) { out.push(cur); cur = ''; } else cur += c;
    }
    if (cur.trim() !== '' || out.length) out.push(cur);
    return out.map((x) => x.trim()).filter((x) => x !== '');
  }

  function parseParams(src) {
    return splitTop(src, ',').map((p) => {
      let s = p.replace(/^this\s+/, '');
      const i = s.search(/\s+[A-Za-z_]\w*$/);
      if (i < 0) return { type: s, name: '' };
      return { type: s.slice(0, i).trim(), name: s.slice(i).trim() };
    });
  }

  const TYPE_RE = '[A-Za-z_]\\w*(?:<[^()]*?>)?\\??';

  function docText(lines) { return lines.map((l) => l.replace(/^\s?/, '')).join('\n').trim(); }

  function paramsText(params) { return params.map((p) => (p.name ? p.type + ' ' + p.name : p.type)).join(', '); }
  function signatureOf(sym) {
    switch (sym.kind) {
      case 'method': return `${sym.static ? 'static ' : ''}method ${sym.name}(${paramsText(sym.params)})${sym.ret ? ': ' + sym.ret : ''}`;
      case 'ctor': return `constructor(${paramsText(sym.params)})`;
      case 'property': return `property ${sym.type} ${sym.name}`;
      case 'field': return `${sym.private ? 'private ' : ''}${sym.static ? 'static ' : ''}${sym.type} ${sym.name}`;
      case 'class': return `${sym.exported ? 'export ' : ''}class ${sym.name}`;
      case 'model': return `${sym.exported ? 'export ' : ''}model ${sym.name}`;
      case 'hand': return `hand:${sym.module} ${sym.name}(${paramsText(sym.params)})${sym.ret ? ': ' + sym.ret : ''}`;
      case 'var': return `${sym.type} ${sym.name}`;
      default: return sym.name;
    }
  }

  // Indexes declarations: { namespace, classes, methods, hands, uses }. Every symbol has {kind, name, line, doc, ...}.
  function indexSource(text) {
    const res = { namespace: '', classes: [], methods: [], hands: [], uses: [], imports: [], locals: [] };
    const raw = text.split(/\r?\n/);
    const scanned = raw.map(scanLine);
    let depth = 0;
    let cls = null;
    let method = null;  // innermost method being walked (for locals)
    let doc = [];
    for (let i = 0; i < raw.length; i++) {
      const { code, comment } = scanned[i];
      const trimmed = code.trim();
      if (trimmed === '') {
        if (comment !== null && depth <= 1) doc.push(comment);
        else if (comment === null) doc = [];
        continue;
      }
      let line = trimmed;
      let consumed = 0;
      // join lines of a declaration whose parameter list is not closed yet
      if (/^(?:(?:public|private|protected|static|override|export)\s+)*(?:method|constructor)\b/.test(line)) {
        while ((line.match(/\(/g) || []).length > (line.match(/\)/g) || []).length && i + consumed + 1 < raw.length) {
          consumed++;
          line += ' ' + scanned[i + consumed].code.trim();
        }
      }
      const d0 = depth;
      let mm;
      if (d0 === 0) {
        if ((mm = /^use\s+"/.exec(line))) {
          const um = /^use\s+"([^"]*)"/.exec(raw[i].trim());
          if (um) res.uses.push({ name: um[1], line: i + 1 });
        } else if ((mm = /^namespace\s+([A-Za-z_][\w.]*)/.exec(line))) {
          if (!res.namespace) res.namespace = mm[1];
        } else if ((mm = /^hand\s*:\s*(\w+)\s+(\w+)\s*\(([^)]*)\)\s*(?::\s*(\w+))?/.exec(line))) {
          res.hands.push({ kind: 'hand', module: mm[1], name: mm[2], params: parseParams(mm[3]), ret: mm[4] || '', line: i + 1, doc: docText(doc) });
        } else {
          let rest = line;
          const mods = new Set();
          for (;;) {
            const w = /^(\w+)\s+/.exec(rest);
            if (w && MODIFIERS.has(w[1])) { mods.add(w[1]); rest = rest.slice(w[0].length); } else break;
          }
          if ((mm = /^(class|model|interface)\s+([A-Za-z_]\w*)\s*(?:<([^>]*)>)?\s*(?::\s*([^{]+?))?\s*(?:\{.*)?$/.exec(rest))) {
            cls = { kind: mm[1], name: mm[2], exported: mods.has('export'), line: i + 1, endLine: raw.length, doc: docText(doc), members: [],
              typeParams: mm[3] ? splitTop(mm[3], ',').map((x) => x.trim()).filter(Boolean) : [], supers: mm[4] ? splitTop(mm[4], ',').map((x) => x.trim()).filter(Boolean) : [] };
            res.classes.push(cls);
            if (mm[1] === 'model') {
              const body = /\{(.*)\}?/.exec(rest);
              if (body) addModelFields(cls, body[1].replace(/\}.*$/, ''), i + 1);
            }
          } else if ((mm = /^method\s+([A-Za-z_]\w*)\s*(?:<[^>]*>)?\s*\(([^)]*)\)\s*(?::\s*([^{]+?))?\s*(?:\{.*)?$/.exec(rest))) {
            const sym = { kind: 'method', name: mm[1], params: parseParams(mm[2]), ret: (mm[3] || '').trim(), exported: mods.has('export'), line: i + 1, endLine: raw.length, doc: docText(doc), extension: /\(\s*this\s/.test(rest) };
            res.methods.push(sym);
            method = sym;
          }
        }
      } else if (cls && d0 === 1) {
        let rest = line;
        const mods = new Set();
        for (;;) {
          const w = /^(\w+)\s+/.exec(rest);
          if (w && MODIFIERS.has(w[1])) { mods.add(w[1]); rest = rest.slice(w[0].length); } else break;
        }
        const base = { line: i + 1, doc: docText(doc), static: mods.has('static'), private: mods.has('private'), protected: mods.has('protected'), override: mods.has('override'), container: cls.name };
        if (cls.kind === 'model' && !/^(method|constructor|property)\b/.test(rest)) {
          addModelFields(cls, rest, i + 1);
        } else if ((mm = /^method\s+([A-Za-z_]\w*)\s*(?:<[^>]*>)?\s*\(([^)]*)\)\s*(?::\s*([^{]+?))?\s*(?:\{.*)?$/.exec(rest))) {
          const sym = { ...base, kind: 'method', name: mm[1], params: parseParams(mm[2]), ret: (mm[3] || '').trim(), endLine: raw.length };
          cls.members.push(sym);
          method = sym;
        } else if ((mm = /^constructor\s*\(([^)]*)\)/.exec(rest))) {
          const sym = { ...base, kind: 'ctor', name: cls.name, params: parseParams(mm[1]), endLine: raw.length };
          cls.members.push(sym);
          method = sym;
        } else if ((mm = new RegExp(`^property\\s+(${TYPE_RE})\\s+([A-Za-z_]\\w*)`).exec(rest))) {
          cls.members.push({ ...base, kind: 'property', name: mm[2], type: mm[1] });
        } else if ((mm = new RegExp(`^(${TYPE_RE})\\s+([A-Za-z_]\\w*)\\s*(?:=|$|;)`).exec(rest))) {
          cls.members.push({ ...base, kind: 'field', name: mm[2], type: mm[1] });
        }
      }
      // local variables and parameters (anywhere in the file; scoped later by line ranges)
      collectLocals(line, i + 1, method, res.locals, d0);
      // brace tracking
      for (const ch of code) {
        if (ch === '{') depth++;
        else if (ch === '}') {
          depth--;
          if (depth === 1 && method && cls) { method.endLine = i + 1; method = null; }
          if (depth === 0) {
            if (method && !cls) { method.endLine = i + 1; method = null; }
            if (cls) { cls.endLine = i + 1; cls = null; method = null; }
          }
        }
      }
      if (depth < 0) depth = 0;
      i += consumed;
      doc = [];
    }
    return res;
  }

  function addModelFields(cls, text, line) {
    const re = new RegExp(`(?:^|[\\s;])(${TYPE_RE})\\s+([A-Za-z_]\\w*)(?=\\s|;|$)`, 'g');
    let m;
    while ((m = re.exec(text))) {
      if (/^(public|private|static|method|property|constructor)$/.test(m[1])) continue;
      cls.members.push({ kind: 'field', name: m[2], type: m[1], line, doc: '', static: false, private: false, container: cls.name });
    }
  }

  function collectLocals(line, lineNo, method, out, depth) {
    let m;
    // parameters of the method/constructor on this line
    if ((m = /^(?:(?:public|private|protected|static|override|export)\s+)*(?:method\s+\w+(?:\s*<[^>]*>)?|constructor)\s*\(([^)]*)\)/.exec(line))) {
      for (const p of parseParams(m[1])) if (p.name) out.push({ kind: 'var', name: p.name, type: p.type.replace(/\.\.\.$/, '[]'), line: lineNo, param: true, scope: method });
    }
    // for (T x in ...)
    const forRe = new RegExp(`\\bfor\\s*\\(\\s*(var|${TYPE_RE})\\s+([A-Za-z_]\\w*)\\s+in\\b`);
    if ((m = forRe.exec(line))) out.push({ kind: 'var', name: m[2], type: m[1], line: lineNo, scope: method });
    // T x = ...  /  var x = ...  (also inside for(...) initializers)
    const declRe = new RegExp(`(?:^|[;{(]\\s*|\\)\\s*\\{\\s*)(var|${TYPE_RE})\\s+([A-Za-z_]\\w*)\\s*=(?!=)`, 'g');
    while ((m = declRe.exec(line))) {
      if (/^(return|else|new|in|is)$/.test(m[1])) continue;
      out.push({ kind: 'var', name: m[2], type: m[1], line: lineNo, scope: method, init: line.slice(m.index + m[0].length) });
    }
  }

  // ---------------------------------------------------------------- libraries and visibility
  // libs: Map(lowercase name -> { name, version, requires, classes, methods, hands, path }). Returns the set of lib names a file can see.
  function visibleLibs(libs, uses) {
    const seen = new Set();
    const walk = (name) => {
      const lib = libs.get(String(name).toLowerCase());
      if (!lib || seen.has(lib.name)) return;
      seen.add(lib.name);
      for (const r of lib.requires) walk(r);
    };
    for (const u of uses) walk(u.name || u);
    return seen;
  }

  function buildLibIndex(parsedLibs) {
    const map = new Map();
    for (const { lib, path } of parsedLibs) {
      if (lib.error) continue;
      const entry = { name: lib.name, version: lib.version, requires: lib.requires, path, classes: [], methods: [], hands: [], files: lib.files.map((f) => f.name), sources: {} };
      for (const f of lib.files) {
        const ix = indexSource(f.content);
        entry.sources[f.name] = f.content;
        for (const c of ix.classes) if (c.exported) { c.lib = lib.name; c.file = f.name; for (const m of c.members) { m.lib = lib.name; m.file = f.name; } entry.classes.push(c); }
        for (const m of ix.methods) if (m.exported) { m.lib = lib.name; m.file = f.name; entry.methods.push(m); }
        for (const h of ix.hands) { h.lib = lib.name; h.file = f.name; entry.hands.push(h); }
      }
      map.set(lib.name.toLowerCase(), entry);
    }
    return map;
  }

  // Where to put `use "<name>"` in a file: after the last use, else after the namespace line / leading comment block.
  // Returns { line, text } for an insertion at the start of `line` (1-based).
  function useInsertion(text, libName) {
    const lines = text.split(/\r?\n/);
    let lastUse = 0;
    for (let i = 0; i < lines.length; i++) if (/^\s*use\s+"/.test(lines[i])) lastUse = i + 1;
    if (lastUse) return { line: lastUse + 1, text: `use "${libName}"\n` };
    let at = 0;
    while (at < lines.length && /^\s*#/.test(lines[at])) at++;
    if (at < lines.length && /^\s*namespace\s/.test(lines[at])) at++;  // a `use` goes below the namespace line
    const next = lines[at];
    const needBlank = next !== undefined && next.trim() !== '';
    return { line: at + 1, text: `use "${libName}"\n` + (needBlank ? '\n' : '') };
  }

  function hasUse(text, libName) {
    const re = new RegExp(`^\\s*use\\s+"${libName.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}"`, 'im');
    return re.test(text);
  }

  // ---------------------------------------------------------------- types and members
  const ARRAY_MEMBERS = [
    { kind: 'property', name: 'length', type: 'int', doc: 'Number of elements.' },
    { kind: 'method', name: 'push', params: [{ type: 'T', name: 'item' }], ret: '', doc: 'Appends an element (statement).' },
    { kind: 'method', name: 'copy', params: [], ret: 'array<T>', doc: 'A shallow copy.' },
  ];
  const MAP_MEMBERS = [
    { kind: 'property', name: 'length', type: 'int', doc: 'Number of entries.' },
    { kind: 'method', name: 'containsKey', params: [{ type: 'K', name: 'key' }], ret: 'boolean', doc: 'true when the key exists.' },
    { kind: 'method', name: 'remove', params: [{ type: 'K', name: 'key' }], ret: 'boolean', doc: 'Removes an entry; true when it existed.' },
    { kind: 'method', name: 'tryGet', params: [{ type: 'K', name: 'key' }], ret: 'V?', doc: 'The value, or null when the key is missing.' },
    { kind: 'method', name: 'keys', params: [], ret: 'array<K>', doc: 'The keys in insertion order.' },
    { kind: 'method', name: 'values', params: [], ret: 'array<V>', doc: 'The values in insertion order.' },
  ];

  function genericArgs(type) {
    const m = /^(\w+)<(.*)>\??$/.exec(type);
    return m ? splitTop(m[2], ',') : [];
  }
  function substitute(sym, map) {
    const sub = (t) => (t ? t.replace(/\b[TKV]\b/g, (x) => map[x] || x) : t);
    return { ...sym, type: sub(sym.type), ret: sub(sym.ret), params: (sym.params || []).map((p) => ({ ...p, type: sub(p.type) })) };
  }
  function builtinMembers(type) {
    const t = type.replace(/\?$/, '');
    const args = genericArgs(t);
    if (/^array</.test(t)) return ARRAY_MEMBERS.map((m) => substitute(m, { T: args[0] || 'T' }));
    if (/^map</.test(t)) return MAP_MEMBERS.map((m) => substitute(m, { K: args[0] || 'K', V: args[1] || 'V' }));
    return [];
  }
  function baseTypeName(type) { return type.replace(/\?$/, '').replace(/<.*$/, ''); }

  // Context for one document: its own symbols plus everything visible from libraries.
  // opts: { libs (Map), workspaceClasses (array), workspaceMethods (array) }
  function analyze(text, opts) {
    const libs = (opts && opts.libs) || new Map();
    const ix = indexSource(text);
    const visible = visibleLibs(libs, ix.uses);
    const classes = new Map();
    const methods = new Map();
    for (const c of (opts && opts.workspaceClasses) || []) classes.set(c.name, c);
    for (const m of (opts && opts.workspaceMethods) || []) methods.set(m.name, m);
    for (const name of visible) {
      const lib = libs.get(name.toLowerCase());
      if (!lib) continue;
      for (const c of lib.classes) classes.set(c.name, c);
      for (const m of lib.methods) methods.set(m.name, m);
    }
    for (const c of ix.classes) classes.set(c.name, c);
    for (const m of ix.methods) methods.set(m.name, m);
    return { ix, visible, classes, methods, libs, text };
  }

  function enclosing(ix, line) {
    for (const c of ix.classes) {
      if (line >= c.line && line <= c.endLine) {
        for (const m of c.members) if ((m.kind === 'method' || m.kind === 'ctor') && line >= m.line && line <= m.endLine) return { cls: c, method: m };
        return { cls: c, method: null };
      }
    }
    for (const m of ix.methods) if (line >= m.line && line <= m.endLine) return { cls: null, method: m };
    return { cls: null, method: null };
  }

  function localsAt(ctx, line) {
    const enc = enclosing(ctx.ix, line);
    const out = new Map();
    for (const v of ctx.ix.locals) {
      if (v.line > line) continue;
      const topLevel = !v.scope;  // declared outside any method: a script global
      if (topLevel || v.scope === enc.method || (enc.method && v.scope && v.scope.line === enc.method.line)) out.set(v.name, v);
    }
    return { vars: out, ...enc };
  }

  // Type of a simple expression: identifier chain with calls and indexers, e.g. `items[0].Name`, `this.list`, `Shell`.
  // Returns { type } for a value, { staticClass } for a class name, or null.
  function typeOfExpr(ctx, expr, line) {
    const segs = [];
    let cur = '';
    let depth = 0;
    for (const c of expr.trim()) {
      if (c === '(' || c === '[') depth++;
      if (c === ')' || c === ']') depth--;
      if (c === '.' && depth === 0) { segs.push(cur); cur = ''; } else cur += c;
    }
    segs.push(cur);
    const scope = localsAt(ctx, line);
    let t = null;
    for (let i = 0; i < segs.length; i++) {
      const seg = segs[i].trim();
      const m = /^([A-Za-z_]\w*)\s*(\(.*\))?\s*((?:\[.*\])*)$/.exec(seg);
      if (!m) return null;
      const name = m[1];
      let cur2 = null;
      if (i === 0) {
        if (name === 'this') cur2 = scope.cls ? { type: scope.cls.name } : null;
        else if (name === 'base' && !m[2]) { const bs = scope.cls && supersOf(ctx, scope.cls).find((x) => x.cls.kind !== 'interface'); cur2 = bs ? { type: bs.type } : null; }
        else if (m[2] && ctx.methods.has(name)) { const r = ctx.methods.get(name).ret; cur2 = r ? { type: r } : null; } else if (scope.vars.has(name)) cur2 = { type: normalizeVarType(ctx, scope.vars.get(name), line) };
        else if (scope.cls && allMembers(ctx, scope.cls).find((x) => x.name === name && x.kind !== 'ctor')) { const mem = allMembers(ctx, scope.cls).find((x) => x.name === name && x.kind !== 'ctor'); cur2 = memberValue(mem); } else if (ctx.classes.has(name)) cur2 = { staticClass: name };
        else cur2 = null;
      } else if (t && t.type) {
        const mem = membersOf(ctx, t.type).find((x) => x.name === name && (m[2] ? x.kind === 'method' || x.kind === 'ctor' : x.kind !== 'method'));
        cur2 = mem ? memberValue(mem) : null;
      } else if (t && t.staticClass) {
        const c = ctx.classes.get(t.staticClass);
        const mem = c && c.members.find((x) => x.name === name && x.static);
        cur2 = mem ? memberValue(mem) : null;
      }
      if (!cur2) return null;
      if (m[3] && cur2.type) {
        for (let k = 0; k < (m[3].match(/\[/g) || []).length; k++) {
          const a = genericArgs(cur2.type);
          if (/^array</.test(cur2.type) && a[0]) cur2 = { type: a[0] }; else return null;
        }
      }
      t = cur2;
    }
    return t;
  }
  function memberValue(mem) {
    if (mem.kind === 'method') return mem.ret ? { type: mem.ret } : null;
    if (mem.kind === 'property' || mem.kind === 'field') return { type: mem.type };
    return null;
  }
  function normalizeVarType(ctx, v, line) {
    if (v.type !== 'var') return v.type;
    const init = (v.init || '').trim();
    let m;
    if ((m = new RegExp(`^new\\s+(${TYPE_RE})`).exec(init))) return m[1];
    if (/^"/.test(init)) return 'string';
    if (/^\d+\.\d+/.test(init)) return 'double';
    if (/^\d+/.test(init)) return 'int';
    if (/^(true|false)\b/.test(init)) return 'boolean';
    if (/^\[/.test(init)) return 'array<obj>';
    if ((m = /^([A-Za-z_]\w*)\s*\(/.exec(init)) && ctx.methods.has(m[1])) return ctx.methods.get(m[1]).ret || 'obj';
    const t = init ? typeOfExpr(ctx, init.replace(/[\s;]+$/, ''), line) : null;
    return t && t.type ? t.type : 'obj';
  }

  // Members reachable on a value of `type` (instance members for classes, built-ins for arrays/maps).
  function substituteAll(sym, map) {
    const keys = Object.keys(map);
    if (!keys.length) return sym;
    const re = new RegExp('\\b(' + keys.join('|') + ')\\b', 'g');
    const sub = (t) => (t ? t.replace(re, (x) => map[x]) : t);
    return { ...sym, type: sub(sym.type), ret: sub(sym.ret), params: (sym.params || []).map((p) => ({ ...p, type: sub(p.type) })) };
  }
  // The direct supertypes of a class as { cls, type } (type keeps its type arguments, e.g. Source<int>).
  function supersOf(ctx, cls) {
    const out = [];
    for (const t of cls.supers || []) { const c = ctx.classes.get(baseTypeName(t)); if (c) out.push({ cls: c, type: t }); }
    return out;
  }
  // All members of a class including inherited ones (own members win over inherited with the same name and parameter count).
  function allMembers(ctx, cls, type, seen) {
    seen = seen || new Set();
    if (seen.has(cls.name)) return [];
    seen.add(cls.name);
    const map = {};
    const args = type ? genericArgs(type) : [];
    (cls.typeParams || []).forEach((p, i) => { if (args[i]) map[p] = args[i]; });
    const own = cls.members.filter((m) => m.kind !== 'ctor').map((m) => substituteAll(m, map));
    const out = [...own];
    const has = (m) => out.some((o) => o.name === m.name && o.kind === m.kind && (o.params || []).length === (m.params || []).length);
    for (const sup of supersOf(ctx, cls)) {
      const supType = sup.type.replace(/\b[A-Za-z_]\w*\b/g, (x) => map[x] || x);
      for (const m of allMembers(ctx, sup.cls, supType, seen)) if (!m.private && !has(m)) out.push({ ...m, inherited: true });
    }
    return out;
  }
  function membersOf(ctx, type) {
    const b = builtinMembers(type);
    if (b.length) return b;
    const c = ctx.classes.get(baseTypeName(type));
    return c ? allMembers(ctx, c, type).filter((m) => !m.static) : [];
  }
  // Methods a class could override (inherited, not private, not static, not overridden yet).
  function overridable(ctx, cls) {
    const own = new Set(cls.members.filter((m) => m.kind === 'method').map((m) => m.name + '/' + m.params.length));
    const out = [];
    for (const sup of supersOf(ctx, cls)) for (const m of allMembers(ctx, sup.cls, sup.type)) if (m.kind === 'method' && !m.static && !m.private && !own.has(m.name + '/' + m.params.length) && !out.some((o) => o.name === m.name && o.params.length === m.params.length)) out.push(m);
    return out;
  }
  // Members reachable through `base.` inside `cls`: those of its base class (not interfaces).
  function baseMembers(ctx, cls) {
    const out = [];
    for (const sup of supersOf(ctx, cls)) if (sup.cls.kind !== 'interface') for (const m of allMembers(ctx, sup.cls, sup.type)) if (!m.static && !m.private) out.push(m);
    return out;
  }
  function staticMembersOf(ctx, className) {
    const c = ctx.classes.get(className);
    return c ? c.members.filter((m) => m.static && !m.private) : [];
  }

  // The call being typed at the end of `prefix` (text of the line up to the cursor): { callee, argIndex } or null.
  function activeCall(prefix) {
    let depth = 0, commas = 0;
    for (let i = prefix.length - 1; i >= 0; i--) {
      const c = prefix[i];
      if (c === ')' || c === ']') depth++;
      else if (c === '[') { if (depth === 0) return null; depth--; } else if (c === '(') {
        if (depth === 0) {
          const m = /([A-Za-z_][\w.]*)\s*$/.exec(prefix.slice(0, i));
          return m ? { callee: m[1], argIndex: commas, open: i } : null;
        }
        depth--;
      } else if (c === ',' && depth === 0) commas++;
      else if (c === '"') { /* strings are rare in the prefix tail; ignore */ }
    }
    return null;
  }

  return { allMembers, overridable, baseMembers, supersOf, KEYWORDS, TYPES, LITERALS, KEYWORD_DOCS, TYPE_DOCS, parseLib, scanLine, splitTop, parseParams, indexSource, signatureOf, paramsText,
    visibleLibs, buildLibIndex, useInsertion, hasUse, builtinMembers, genericArgs, baseTypeName, analyze, enclosing, localsAt, typeOfExpr, membersOf,
    staticMembersOf, activeCall, normalizeVarType };
});

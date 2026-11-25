#!/usr/bin/env python3
"""Migrates Coyote source from 0.6.4 to 0.6.5.

    python3 tools/migrate-0.6.5.py file.coy [more files or folders ...]

* Text.Trim(s)            ->  s.Trim()              (the static class Text was removed; the string extensions stay)
* Processes.Kill(id) / IsRunning(id) / CurrentId()  ->  Process.FromId(id).Kill() / .IsRunning() / Process.CurrentId()
  (Processes.Start / Processes.Run took one argument string: change them by hand to Process.Start(program, [args]) / Process.Run)
* Runtime.OS() / Runtime.UptimeMs()  ->  Environment.OS() / Time.UptimeMs()
* Regex.Create(p, options)  ->  new Regex(p, options)  (constructors can be overloaded now)
* ResourceFolder.Names(f) ->  Resources.NamesIn(f)

Files are rewritten in place (a .bak copy is not made: use version control)."""
import re, sys, os

TEXT_FUNCS = "Length IsEmpty IsBlank Substring CharAt IndexOf Contains StartsWith EndsWith Upper Lower Trim Replace Repeat PadLeft PadRight Reverse Split Join".split()
PAT = re.compile(r'\bText\.(' + '|'.join(TEXT_FUNCS) + r')\(')

def split_args(src, i):
    """src[i] is just after '('. Returns (args, index after ')') or None."""
    depth, args, cur, j = 0, [], '', i
    while j < len(src):
        c = src[j]
        if c == '"':
            k = j + 1
            while k < len(src) and src[k] != '"':
                k += 2 if src[k] == '\\' else 1
            cur += src[j:k + 1]; j = k + 1; continue
        if c in '([{': depth += 1
        elif c in ')]}':
            if depth == 0:
                args.append(cur.strip()); return args, j + 1
            depth -= 1
        elif c == ',' and depth == 0:
            args.append(cur.strip()); cur = ''; j += 1; continue
        cur += c; j += 1
    return None

SIMPLE = re.compile(r'^(?:[A-Za-z_][\w]*(?:\[[^\]]*\]|\([^()]*\))*(?:\.[A-Za-z_]\w*(?:\[[^\]]*\]|\([^()]*\))*)*|"(?:[^"\\]|\\.)*")$')

def fix_text(src):
    out, pos = '', 0
    while True:
        m = PAT.search(src, pos)
        if not m: return out + src[pos:]
        r = split_args(src, m.end())
        if not r or not r[0]:
            out += src[pos:m.end()]; pos = m.end(); continue
        args, end = r
        recv = fix_text(args[0]); rest = [fix_text(a) for a in args[1:]]
        if not SIMPLE.match(recv): recv = '(' + recv + ')'
        out += src[pos:m.start()] + recv + '.' + m.group(1) + '(' + ', '.join(rest) + ')'
        pos = end

SIMPLE_RENAMES = [
    (r'\bProcesses\.CurrentId\(', 'Process.CurrentId('),
    (r'\bProcesses\.(IsRunning|Kill)\(([^()]*)\)', r'Process.FromId(\2).\1()'),
    (r'\bRegex\.Create\(', 'new Regex('),
    (r'\bRuntime\.OS\(', 'Environment.OS('),
    (r'\bRuntime\.UptimeMs\(', 'Time.UptimeMs('),
    (r'\bResourceFolder\.Names\(', 'Resources.NamesIn('),
]

def migrate(src):
    src = fix_text(src)
    for a, b in SIMPLE_RENAMES: src = re.sub(a, b, src)
    return src

def main(paths):
    for p in paths:
        files = [os.path.join(d, f) for d, _, fs in os.walk(p) for f in fs if f.endswith('.coy')] if os.path.isdir(p) else [p]
        for f in files:
            s = open(f, encoding='utf-8').read(); n = migrate(s)
            if n != s: open(f, 'w', encoding='utf-8').write(n); print('migrated', f)

if __name__ == '__main__':
    main(sys.argv[1:])

#!/usr/bin/env python3
"""Migrates Coyote source from 0.7 to 0.8.

    python3 tools/migrate-0.8.py file.coy [more files or folders ...]

* catch (string e)  ->  catch (e)
* inside a catch block, a bare use of the caught variable as text (e, not e.Something, not `throw e`)  ->  e.Message
  (in 0.8 `catch (e)` gives an Exception object; "text" + e would also work and prints "Error: text", so review the result)

Files are rewritten in place (no .bak copy: use version control). Review the diff: the script is a text heuristic."""
import re, sys, os

CATCH = re.compile(r'catch\s*\(\s*(?:string\s+)?([A-Za-z_]\w*)\s*\)\s*\{')

def block_end(src, i):
    """src[i] is just after '{'. Returns the index of the matching '}' (strings and # comments skipped)."""
    depth, j = 1, i
    while j < len(src):
        c = src[j]
        if c == '"':
            j += 1
            while j < len(src) and src[j] != '"':
                j += 2 if src[j] == '\\' else 1
        elif c == '#':
            while j < len(src) and src[j] != '\n': j += 1
        elif c == '{': depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0: return j
        j += 1
    return len(src)

def fix_body(body, v):
    out, i = [], 0
    pat = re.compile(r'(?<![\w.])' + re.escape(v) + r'(?![\w(.:])')
    while i < len(body):
        c = body[i]
        if c == '"':
            j = i + 1
            while j < len(body) and body[j] != '"':
                j += 2 if body[j] == '\\' else 1
            out.append(body[i:j + 1]); i = j + 1; continue
        if c == '#':
            j = body.find('\n', i); j = len(body) if j < 0 else j
            out.append(body[i:j]); i = j; continue
        m = pat.match(body, i)
        if m and not re.search(r'throw\s+$', ''.join(out)):
            out.append(v + '.Message'); i = m.end(); continue
        out.append(c); i += 1
    return ''.join(out)

def migrate(src):
    res, pos = [], 0
    for m in CATCH.finditer(src):
        if m.start() < pos: continue
        end = block_end(src, m.end())
        res.append(src[pos:m.start()])
        res.append('catch (' + m.group(1) + ') {')
        res.append(fix_body(src[m.end():end], m.group(1)))
        pos = end
    res.append(src[pos:])
    return ''.join(res)

def files(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, _, names in os.walk(p):
                for n in names:
                    if n.endswith('.coy'): yield os.path.join(root, n)
        else:
            yield p

for f in files(sys.argv[1:]):
    s = open(f, encoding='utf-8').read()
    t = migrate(s)
    if t != s:
        open(f, 'w', encoding='utf-8', newline='').write(t)
        print('migrated', f)

import re, sys

SKIP_FIRST = {"method","constructor","property","field","override"}
MODS = {"public","hidden","protected","shared","private","static","readonly","locked"}

def mask(line):
    """Return line with string contents and the trailing # comment blanked (same length)."""
    out=[]; i=0; instr=None
    while i < len(line):
        c=line[i]
        if instr:
            if c=='\\' and i+1<len(line):
                out.append('  '); i+=2; continue
            if c==instr: instr=None; out.append(c)
            else: out.append(' ')
        else:
            if c in '"\'':
                instr=c; out.append(c)
            elif c=='#':
                out.append(' '*(len(line)-i)); break
            else: out.append(c)
        i+=1
    return ''.join(out)

def migrate(text):
    lines=text.split('\n'); res=[]
    depth=0; classDepths=[]  # stack of depths at which class bodies' members live
    pending_class=False
    for line in lines:
        m=mask(line)
        # --- renames ---
        def sub_words(l, m):
            # operate on positions in masked text
            out=list(l)
            edits=[]
            for mo in re.finditer(r'\b(private|static|this)\b', m):
                edits.append((mo.start(), mo.end(), {'private':'hidden','static':'shared','this':'here'}[mo.group(1)]))
            mo=re.match(r'(\s*)export\b', m)
            if mo: edits.append((mo.end()-6, mo.end(), 'public'))
            # foreach
            fm=re.search(r'\bfor(\s*)\((\s*)(var|[\w<>?,\. ]+?)(\s+)(\w+)(\s+)in(\s)', m)
            if fm and not re.search(r';', m[fm.start():m.find(')', fm.start()) if m.find(')', fm.start())>0 else len(m)]):
                s=fm.start()
                edits.append((s, s+3, 'in'))
                ins=m.index(' in', fm.start(5)) if False else None
                # the 'in' keyword position
                pos=fm.start(6)+len(fm.group(6))
                edits.append((pos, pos+2, 'of'))
            for a,b,r in sorted(edits, reverse=True):
                l=l[:a]+r+l[b:]
                m=m[:a]+r+m[b:]
            return l,m
        line,m=sub_words(line,m)
        # --- fields inside class bodies ---
        st=m.strip()
        if classDepths and depth==classDepths[-1] and st and not st.startswith('}'):
            toks=re.findall(r'\w+|\S', st)
            k=0
            while k < len(toks) and toks[k] in MODS: k+=1
            if k < len(toks) and toks[k] not in SKIP_FIRST and re.match(r'\w', toks[k]):
                # insert 'field ' before the type token
                idx=0; cnt=0
                for mo in re.finditer(r'\w+|\S', m):
                    if cnt==k: idx=mo.start(); break
                    cnt+=1
                line=line[:idx]+'field '+line[idx:]
                m=m[:idx]+'field '+m[idx:]
        # --- brace tracking ---
        if re.match(r'\s*((public|shared|hidden)\s+)*(class|model|interface)\b', m) :
            pending_class=True
        for ch in m:
            if ch=='{':
                depth+=1
                if pending_class:
                    classDepths.append(depth); pending_class=False
            elif ch=='}':
                if classDepths and depth==classDepths[-1]: classDepths.pop()
                depth-=1
        res.append(line)
    return '\n'.join(res)

if __name__=='__main__':
    for p in sys.argv[1:]:
        s=open(p,encoding='utf-8').read()
        n=migrate(s)
        if n!=s: open(p,'w',encoding='utf-8',newline='').write(n); print('migrated',p)

#!/usr/bin/env python3
"""Mutation fuzzer for the Coyote toolchain (not part of the build; run it by hand or in a nightly job).

  tools/fuzz.py ml <pvm-binary> <seed.ml>...    mutate ML text, run the PVM
  tools/fuzz.py ct <coyote-binary> <seed.coy>... mutate source text, run `coyote check`

Every run must exit with code 0 or 1 (success or a clean diagnostic). Any other exit code - a
signal, a sanitizer abort, a usage error - is reported as a CRASH and the input is kept as
crash<N>.ml / crash<N>.coy in the current directory. Timeouts are counted separately: mutated ML can
legitimately loop forever (ML has no instruction limit). Build the target with
-fsanitize=address,undefined to also catch silent memory errors.

Optional environment: FUZZ_RUNS (default 3000), FUZZ_SEED (default 1).
"""
import os, random, subprocess, sys, tempfile

mode, binary, seeds = sys.argv[1], sys.argv[2], [open(p, encoding="utf-8").read() for p in sys.argv[3:]]
runs, rnd = int(os.environ.get("FUZZ_RUNS", "3000")), random.Random(int(os.environ.get("FUZZ_SEED", "1")))
ml_words = ["load", "store", "add.int", "pop", "ret", "jump", "jump.false", "call", "hostcall", "concat",
            "str.int", "not", "push.int", "push.str", "push.bool", "label", "local", "div.int", "eq.str",
            "0", "1", "-1", "99999999999", '"x"', "true", "L0", "L1", "main", "(", ")", "->", "int", "string", "boolean", "newarray", "arraylit", "aload", "astore", "alen", "apush", "acopy", "eq.ref", "array<int>", "array<array<int>>"]
ct_chars = list('(){}=+-*/%!<>&|,;:."#\n \t') + ["method ", "var ", "if ", "else ", "while ", "return ",
            "hand:", "true", "false", "int ", "string ", "boolean ", '"', "\\", "0", "2147483648", "é", "array<int>", "new ", "[", "]", ".length", ".push(", ".copy()"]

def mutate_ml(text):
    lines = text.split("\n")
    for _ in range(rnd.randint(1, 4)):
        k, r = rnd.randrange(len(lines)), rnd.random()
        if r < .2: del lines[k]
        elif r < .4: lines.insert(k, lines[rnd.randrange(len(lines))])
        elif r < .55:
            j = rnd.randrange(len(lines)); lines[k], lines[j] = lines[j], lines[k]
        else:
            w = lines[k].split(" ")
            w[rnd.randrange(len(w))] = rnd.choice(ml_words)
            lines[k] = " ".join(w)
        if not lines: lines = [""]
    return "\n".join(lines)

def mutate_ct(text):
    s = list(text)
    for _ in range(rnd.randint(1, 6)):
        if not s: break
        k, r = rnd.randrange(len(s)), rnd.random()
        if r < .3: del s[k]
        elif r < .6: s.insert(k, rnd.choice(ct_chars))
        elif r < .8: s[k] = rnd.choice(ct_chars)
        else:
            j = rnd.randrange(len(s)); s[k:k + rnd.randint(1, 20)] = s[j:j + rnd.randint(1, 20)]
    return "".join(s)

mutate, ext = (mutate_ml, "ml") if mode == "ml" else (mutate_ct, "ct")
argv0 = [binary] if mode == "ml" else [binary, "check"]
stats = {"exit0": 0, "exit1": 0, "timeout": 0, "CRASH": 0}
with tempfile.TemporaryDirectory() as tmp:
    path = os.path.join(tmp, "input." + ext)
    for n in range(runs):
        data = mutate(rnd.choice(seeds))
        with open(path, "w", encoding="utf-8", errors="surrogateescape") as f: f.write(data)
        try:
            p = subprocess.run(argv0 + [path], capture_output=True, timeout=5)
        except subprocess.TimeoutExpired:
            stats["timeout"] += 1; continue
        if p.returncode in (0, 1): stats["exit%d" % p.returncode] += 1
        else:
            stats["CRASH"] += 1
            keep = "crash%d.%s" % (n, ext)
            with open(keep, "w", encoding="utf-8", errors="surrogateescape") as f: f.write(data)
            print("exit code", p.returncode, "->", keep, p.stderr[:200])
print(stats)
sys.exit(1 if stats["CRASH"] else 0)

# cdt — developer tools

```
cdt check [--json] <file.coy>             diagnostics (text on stderr, or one JSON array on stdout)
cdt tokens <file.coy>                     token stream
cdt ast <file.coy>                        syntax tree (before semantic analysis)
cdt fmt [--check|--write] <file.coy>...   formatter
cdt verify-ml <file.ml>                  verify ML with the PVM without running it
cdt doctor                               check the installed toolchain
```

Exit codes: 0 ok; 1 problems found (diagnostics, files that need formatting, a required tool
missing); 2 usage or I/O error.

## check --json

```json
[
 {"file": "t.coy", "line": 2, "column": 9, "severity": "error", "message": "unknown variable 'zzz'"}
]
```

Lines and columns are 1-based. An empty array means no problems. This is what the VS Code
extension consumes.

## Formatter

`cdt fmt` is deliberately conservative. It re-indents by brace depth (4 spaces), strips trailing
whitespace, collapses runs of blank lines to one, removes blank lines at the start and end,
converts line endings to `\n`, and ends the file with a newline. It **does not touch anything
inside a line** — no spacing around operators, no line breaking — and comments are kept as written.
Before returning, it tokenizes its own output and refuses (with an error, writing nothing) if the
token stream differs from the input. A file that does not tokenize is reported, not formatted.
A full syntax-tree-based pretty printer (which needs comments in the AST) is future work.

`--check` lists files that would change and exits 1; `--write` rewrites only files that change.

## doctor

Runs `--version` on `coyote`, `pvm`, `cpx` and `cpm` next to `cdt`, notes whether the optional
`coyote-llvm` and `git` are present, and exits 1 if a required tool is missing.

## VS Code

See `editors/vscode/`: highlighting, snippets, diagnostics (from `check --json`) and *Format
Document* (from `fmt`). It has not been run inside VS Code in this build environment.

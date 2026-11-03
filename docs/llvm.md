# Native backend: `coyote-llvm`

Status: **every program builds to an executable.** Programs inside a small, tested subset become real LLVM/clang
native code; every other program becomes a *bundled executable* (see below), so `Native` never fails just because
a program uses doubles, arrays, classes or a library. It is a separate tool from the compiler and the PVM. Its input is
verified ML (`docs/ml-spec.md`), exactly what the PVM runs, so `.coy` programs go
`coyote build x.coy -o x.ml` then `coyote-llvm x.ml -o x`.

```
coyote-llvm [--emit-ll] [--bundle] [--native-only] [-o <file>] [--clang <path>] <program.ml>
```

| Mode | What you get |
|---|---|
| default | inside the native subset: LLVM IR + clang -> native code. Outside it: a bundled executable, with a `note:` line on stderr saying why |
| `--bundle` | always a bundled executable (no clang needed) |
| `--native-only` | native code or an error (exit 1) — never a bundle; useful in tests |
| `--emit-ll` | LLVM IR only (never bundles; unsupported programs are an error) |

### Bundled executables

A bundled executable is a copy of `pvm` with the program's ML text appended (`[ML][8-byte length]["CYBNDL01"]`).
When it starts, `pvm` finds that trailer in its own file and runs the embedded program: **any** program works (all
types, classes, inheritance, generics, every `Standard` library), it is a single `.exe` that needs nothing installed,
and its behavior is identical to `coyote run` — the same PVM, so garbage collection, call-depth limits and error
messages are the same. It runs with every permission (it is your own program) and unbuffered output, and the command
line is passed to the program (`Environment.Arg`). It is not faster than the PVM; use the native subset for that.

* Default: writes LLVM IR and a small C runtime to a temporary directory and runs `clang -O2` on them.
  `clang` must be installed (not needed with `--emit-ll`, which writes the textual IR, `-o -` for stdout, nor for
  bundled executables). The Windows setup installs it (component "Native builds", `<install>\clang`).
* Exit codes: 0 ok; 1 ML rejected by the verifier (or, with `--native-only`/`--emit-ll`, the program is outside the
  native subset); 2 usage, I/O or clang failure.
* No LLVM libraries are linked: the tool prints IR text, so it builds without LLVM installed.

## What is supported

* Types: `int` (i32), `boolean`, `string` (NUL-terminated bytes; constants containing NUL are rejected).
* `int` arithmetic with the PVM's checks (`integer overflow`, `division by zero`, `remainder by zero`),
  comparisons, string `==`/`!=`, concatenation, `string(int)`, `string(boolean)`, `!`.
* Locals, parameters, calls (recursion included), `if`/`while`/`for`/`break`/`continue`/`return`.
* Host functions: only `con.print` and `con.println` (console). Runtime errors print
  `<file>.coy:<line>: runtime error: <message>` to stderr and exit 1, like the PVM.

## What the native (clang) path does not support (such programs are bundled instead)

`double`, `char`, `Guid`, arrays, maps, nullable types, `obj`/`dyn`, classes and globals, any other
host function, and a backward jump to a label not reached by fall-through (the compiler never
generates one).

## Known differences from the PVM

* No garbage collector: concatenation and `string(int)` allocate and never free. Long-running string
  loops will grow memory.
* No call-depth limit: runaway recursion crashes the process (stack overflow) instead of printing
  `call stack overflow`.
* Host permissions are fixed at build time (console only); there is no `--allow`.
* Only verified on Linux with clang 18. Windows quoting of the clang command is untested.

## How it is tested

In git history (before the tests were removed), `tests/cases/native/*.coy` were compiled to ML and run both by the PVM and natively; stdout, stderr and the
exit code must be identical, and stdout must equal a hand-written expected file (`native.*` in CTest, only
registered when `clang` is found). Unsupported programs are checked for their exact error text.
The tests are small (5 cases); there has been no fuzzing of this backend. `bundle.*` cases build bundled executables
(inheritance, generics, dead-code removal) and compare their output with the expected files.

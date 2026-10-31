# Native backend: `coyote-llvm`

Status: **a small, tested subset.** It is a separate tool from the compiler and the PVM. Its input is
verified ML (`docs/ml-spec.md`), exactly what the PVM runs, so `.coy` programs go
`coyote build x.coy -o x.ml` then `coyote-llvm x.ml -o x`.

```
coyote-llvm [--emit-ll] [-o <file>] [--clang <path>] <program.ml>
```

* Default: writes LLVM IR and a small C runtime to a temporary directory and runs `clang -O2` on them.
  `clang` must be installed (not needed with `--emit-ll`, which writes the textual IR, `-o -` for stdout).
* Exit codes: 0 ok; 1 ML rejected by the verifier, or the program uses something unsupported;
  2 usage, I/O or clang failure.
* No LLVM libraries are linked: the tool prints IR text, so it builds without LLVM installed.

## What is supported

* Types: `int` (i32), `boolean`, `string` (NUL-terminated bytes; constants containing NUL are rejected).
* `int` arithmetic with the PVM's checks (`integer overflow`, `division by zero`, `remainder by zero`),
  comparisons, string `==`/`!=`, concatenation, `string(int)`, `string(boolean)`, `!`.
* Locals, parameters, calls (recursion included), `if`/`while`/`for`/`break`/`continue`/`return`.
* Host functions: only `con.print` and `con.println` (console). Runtime errors print
  `<file>.coy:<line>: runtime error: <message>` to stderr and exit 1, like the PVM.

## What is not supported (rejected at build time, naming the function and ML line)

`double`, `char`, `Guid`, arrays, maps, nullable types, `obj`/`dyn`, classes and globals, any other
host function, and a backward jump to a label not reached by fall-through (the compiler never
generates one). Use the PVM for those.

## Known differences from the PVM

* No garbage collector: concatenation and `string(int)` allocate and never free. Long-running string
  loops will grow memory.
* No call-depth limit: runaway recursion crashes the process (stack overflow) instead of printing
  `call stack overflow`.
* Host permissions are fixed at build time (console only); there is no `--allow`.
* Only verified on Linux with clang 18. Windows quoting of the clang command is untested.

## How it is tested

`tests/cases/native/*.coy` are compiled to ML and run both by the PVM and natively; stdout, stderr and the
exit code must be identical, and stdout must equal a hand-written expected file (`native.*` in CTest, only
registered when `clang` is found). Unsupported programs are checked for their exact error text.
The tests are small (5 cases); there has been no fuzzing of this backend.

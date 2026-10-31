# Architecture

```
 .coy source
     |
     v
 [ lexer ] -> [ parser ] -> AST -> [ sema ] -> [ ML generator ]      src/compiler  ->  `coyote`
                                                      |
                                           textual ML (docs/ml-spec.md)         <- the only interface
                                                      |
                                                      v
                              [ loader ] -> [ verifier ] -> [ interpreter ]      src/pvm  ->  `pvm`
                                                                ^
                                                      host registry + permissions
```

## Boundaries

* `src/compiler` and `src/pvm` are separate CMake libraries with **no include or link dependency
  on each other**. The test suite and CI run them as separate processes connected only by a
  `.ml` file.
* The PVM never needs LLVM. The native backend `coyote-llvm` is a separate tool: it reads ML (via the PVM's loader/verifier library), emits textual LLVM IR and calls `clang`. See `llvm.md`.
* Platform-specific code is not needed yet; everything is portable C++17 and builds with GCC,
  Clang and MSVC.

## Compiler (`src/compiler`)

| File | Role |
|------|------|
| `lexer.*` | bytes to tokens; first lexical error aborts |
| `parser.*` | tokens to AST; first syntax error aborts; rejects reserved-but-unimplemented words |
| `sema.*` | scopes, type checking, resolves calls to `hand` declarations and methods, checks that non-void methods return on every path; collects *all* semantic errors without cascades |
| `mlgen.*` | AST to ML text with `.line` mapping |
| `main.cpp` | CLI: `coyote build|check` |

Exit codes: 0 success, 1 compile errors, 2 usage or I/O errors. No output file is written when
compilation fails.

## PVM (`src/pvm`)

| File | Role |
|------|------|
| `loader.*` | ML text to `Module`; syntax-level errors (`ml error`) |
| `verifier.*` | whole-module checks before execution (`verification error`), including operand-stack typing over every control-flow path |
| `host.*` | registry of permitted host functions; console hosts |
| `heap.*` | managed heap and garbage collector |
| `interpreter.*` | typed operand-stack interpreter with explicit call frames (no native recursion); runtime errors mapped to `.coy` lines |
| `main.cpp` | CLI: `pvm [--verify-only] [--allow perm] [--heap-limit n] [--gc-stress] [--gc-stats] file.ml` |

## Project tools (`src/pkg`, `src/cpx`)

| File | Role |
|------|------|
| `pkg/json.*` | strict JSON parser with line/column positions (depth and duplicate-key checks) |
| `pkg/manifest.*` | `packages.json` schema v1 validation, default workflows, reference/cycle checks |
| `pkg/process.*` | runs a program without a shell (POSIX `posix_spawn`; Windows `CreateProcessW`) and reports its exit status |
| `cpx/main.cpp` | CLI: `cpx <workflow>`, `list`, `check` ([semantics](cpx.md)) |

These libraries depend on neither the compiler nor the PVM; `cpx` only starts `coyote` and `pvm` as
separate processes.

### Garbage collection

`heap.*` implements a precise, non-moving, stop-the-world mark-and-sweep collector over array and
map objects (`Obj`). Marking is iterative, so deeply nested arrays, maps and boxed values cannot
overflow the native stack. Boxed (`obj`) values are reference-counted immutable cells that hold
heap references; the collector marks through them.

* **Roots:** the operand stack and the slots of every call frame (null array references are
  skipped). Nothing else holds heap pointers.
* **When:** only inside an allocating instruction, while its operands are still on the stack. The
  next collection triggers at max(floor, 2x live) objects or elements.
* **Limit:** `--heap-limit` live elements; over the limit after a collection is a runtime error.
* **Testing:** `--gc-stress` collects before every allocation. Every positive test also runs with
  it, and the `gc` tests assert that collections happened and garbage was freed. Removing the
  frame-slot roots in a scratch copy makes these tests fail.
* **Not covered yet:** objects with finalizers or native resources, closures. Class instances and globals are covered (globals are roots).
  Their lifetimes will be designed with those features.

## Testing

`tests/` drives the real binaries through CTest. Case kinds: `ok` (compile+run, stdout compared),
`err` (compile must fail, diagnostics compared), `ml_ok`, `ml_err` (hand-written ML the PVM must
accept or reject, output or diagnostics compared), `gc` (as `ok`, plus the collector must have run and freed garbage), `ct_pvm_err` (valid source that fails in the PVM,
e.g. a runtime error or an unregistered host function; diagnostics compared). The shipped
`samples/hello.coy` is itself a test. CI additionally runs Hello world and an invalid-source check
against the *installed* binaries.

`tools/fuzz.py` is a mutation fuzzer for the PVM (mutated ML) and the compiler (mutated source). It
is run by hand; every run must end in exit code 0 or 1, and anything else is a crash.

<p align="center">
  <img src="assets/brand/cayote-logo-showcase.png" alt="Coyote logo" width="420">
</p>

# Coyote

Coyote is a programming language and toolchain: a C++ compiler that produces a textual,
versioned intermediate language (ML), and the Pixel Virtual Machine (PVM) that verifies and runs
it. Source files use the `.coy` extension.

```
# samples/fibonacci.coy
hand:con println(string text)

method fib(int n) : int {
    if (n < 2) { return n }
    return fib(n - 1) + fib(n - 2)
}

var i = 0
while (i < 10) {
    println("fib(" + i + ") = " + fib(i))
    i = i + 1
}
```

```
$ coyote build samples/fibonacci.coy -o fib.ml
$ pvm fib.ml
fib(0) = 0
fib(1) = 1
...
fib(9) = 34
```

`samples/hello.coy` is the standard "Hello world" (`use "Standard"` + `Shell.PrintLine`); `samples/hello_host.coy` does the same with a raw `hand:` host declaration and no library.

## Implementation status

Coyote is an early work in progress. Only what is marked "working" exists.

| Component | Status |
|-----------|--------|
| Compiler front end (`coyote`) | **Working** for a subset: `int`/`string`/`boolean`/`double`/`char`/`Guid`, `T?` and `null` (`??`, `!`), `obj`, `dyn`, `map<K,V>`, `var`, assignment, expressions (arithmetic, comparison, short-circuit logic, string concatenation), `if`/`else`/`while`/`for`, `import`/`export` of methods across files and installed packages, `for … in` over arrays, `break`/`continue`, compound assignment, explicit conversions (`int(x)`, `string(x)`, …), `method` with parameters, return values and recursion, `array<T>` (literals, `new`, indexing, `length`, `push`, `copy`), `hand:` host declarations ([language](docs/language.md)) |
| ML v5/v6 generation with `.coy` line mapping | **Working** (v6 = function values: the PVM runs them, the compiler does not emit them yet) ([spec](docs/ml-spec.md)); the PVM also accepts ML v1 to v4 |
| ML loader and verifier | **Working**, including operand-stack typing across branches and loops |
| PVM interpreter, call frames, host registry with permissions | **Working** (`con.print`, `con.println`); runtime errors for division by zero, `int` overflow, call depth, array bounds/null/size, heap limit |
| Classes, models, constructors, properties, static fields/methods, `public`/`private`/`protected` | **Working** (ML v5 `class`/`global`/`newobj`/`getfield`...; no overloading) ([docs](docs/language.md)) |
| Inheritance, interfaces, `override`/`base`, `as`/`is` | **Working** (single base class + interfaces, virtual methods through compiler-generated dispatchers; ML v7 only for programs that use it) ([docs](docs/language.md)) |
| Generics (classes, interfaces, top-level methods) | **Working** by compile-time expansion; type arguments are explicit (`First<int>(xs)`), no inference, no generic methods inside classes ([docs](docs/language.md)) |
| Dead-code removal | **Working** (unreachable methods and classes are dropped from the ML; `--keep-unused` disables) ([docs](docs/compiler.md)) |
| Variadic parameters, extension methods | **Working** (compile-time rewrites; [docs](docs/language.md)) |
| Libraries: `use`, `.lib` files, Standard libraries (`Standard.IO`, `.Math`, `.Text` with extension methods, `.Time`, `.Random`, `.Collections`, `.Testing`, HTTP, ...), host functions with return values | **Working**; `.lib` is a **source bundle** (compiled/linked libs are not implemented) ([docs](docs/libraries.md)) |
| Closures/lambdas, LINQ-like APIs | Not implemented (lambdas need function values in ML, which do not exist yet; LINQ-like APIs depend on them) |
| Garbage collector | **Working** for arrays, maps, boxed values and class instances: precise, non-moving mark-and-sweep; `--gc-stress`, `--gc-stats`, `--heap-limit` ([design](docs/architecture.md)). Strings are copied by value and are not on the heap. Nothing with finalizers or native resources exists yet |
| LLVM native backend (`coyote-llvm`) | **Working for a small subset** (int/boolean/string, calls, control flow, console print); outside the subset it falls back to a **bundled executable** (the PVM plus the program) unless `--native-only`; no GC, strings leak ([docs](docs/llvm.md)). The installer ships clang (llvm-mingw); **not yet verified on Windows** |
| `cpx` (workflows, `.cyproj` / `packages.json` schema v1) | **Working**: `cpx start`/`build`/`list`/`check`, project output and run settings from the project file ([docs](docs/cpx.md), [projects](docs/projects.md)); dependencies are rejected as not implemented. Windows process code is written but never compiled on Windows |
| `cpm` (dependencies, lock file) | **Working** for `path` and `git` dependencies: deterministic resolution, integrity-checked lock file, `install`/`update`/`lock`/`verify`/`init`/`layout` ([docs](docs/cpm.md)). `url`/registry providers are interfaces only. Installed packages are consumed through `import "name"` |
| `cdt` (check/tokens/ast/fmt/doctor) | **Working** ([docs](docs/cdt.md)); the formatter is conservative (re-indents, never changes tokens) |
| Coyote Studio (desktop IDE) + debugger | **Working on Linux under Electron**: editor with context-aware completion (imports, inheritance, generics), Project Manager (`.cyproj`), Build/Rebuild/Clean, separate app window for debugging, Settings page with themes, `pvm --debug` debugger; UI-tested. The Windows package is built by CI and **never run on Windows** ([studio](docs/studio.md), [debugging](docs/debugging.md)) |
| Windows installer (`Coyote-Setup-<version>.exe`) | Script compiled and lifecycle-tested under Wine (install, update detection, downgrade refusal, repair, uninstall); the CI job producing it **has not run** ([studio](docs/studio.md)) |
| VS Code extension | Written and unit-tested (diagnostic mapping, grammar scopes with the real TextMate engine); **never loaded in VS Code**; no language server ([docs](editors/vscode/README.md)) |
| `coyote` as unified entry point | **Working**: `build`, `check`, `run`, and `coyote pvm|cpx|cpm|cdt|llvm ...` forward to the tool next to it |
| GitHub Actions workflow | Written ([ci.yml](.github/workflows/ci.yml)); **not yet run on GitHub** |

Known limits: ML has no instruction or time limit, so a verified program can loop forever;
`method` syntax is provisional. Input limits: source 4 MiB, ML 32 MiB, nesting 256, expression depth 1000.

### What has been verified

Locally, on Linux with GCC 13 and Clang 18 (warnings as errors): the build, and 314 CTest cases (including 42 `cpx`, 17 `cpm`, 14 `cdt`, 10 multi-file module cases and 5 native-backend differential cases)
that run the real binaries: programs with hand-computed expected output (each also run with a
collection before every allocation), compile-error cases, valid/rejected hand-written ML, runtime
errors, GC tests that assert collections happened and garbage was freed, and generated
hostile-input cases (deep nesting, huge chains, oversized files). The same suite passes under
AddressSanitizer + UBSan. Removing the frame-slot GC roots in a scratch copy makes the GC tests
fail, so they do detect that bug class. `tools/fuzz.py` ran 1,500 mutated ML programs (seeded from array, map, nullable, class and variadic programs)
and 1,500 mutated sources under the sanitizers with no crashes (the native backend is not fuzzed); mutated ML can
legitimately hang, which is reported as a timeout, not a crash.

Not verified: macOS, Windows, ARM (CI matrix entries that have not run), and any GitHub Actions run.

## Build

Requirements: CMake 3.20+, a C++17 compiler (GCC, Clang or MSVC). No other dependencies.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/coyote build samples/hello.coy -o hello.ml && ./build/pvm hello.ml
```

(On Windows with Visual Studio generators add `--config Release` to build and `-C Release` to ctest.)

## CI

`.github/workflows/ci.yml` builds with the tests switched off (`-DCOYOTE_BUILD_TESTS=OFF`) because the repository carries no tests (see [FORAI.md](FORAI.md)). It builds on Linux x86_64, Linux arm64, macOS arm64 and Windows x86_64, uploads one archive per platform, packages the VS Code extension, and builds the Windows installer (`Coyote-Setup-<version>.exe`). On a pushed tag `v*` it publishes a GitHub release with all of these and a `SHA256SUMS.txt`.

There is no test suite in the repository on purpose: it kept growing and slowed down AI-assisted work. [FORAI.md](FORAI.md) explains the policy (write only the tests relevant to the change at hand). The full suite remains in git history (commit `97453e2` and earlier: `git show 97453e2:tests/CMakeLists.txt`, `git checkout 97453e2 -- tests`).

* Permissions are read-only except the release job (`contents: write`).
* Actions are pinned to exact version tags, **not yet commit SHAs**.
* Nothing in this workflow has been verified by a successful run yet.

## Documentation

* [Language (what is implemented)](docs/language.md)
* [Libraries, `use` and the Standard library](docs/libraries.md)
* [Projects and `.cyproj` files](docs/projects.md)
* [Compiler options and dead-code removal](docs/compiler.md)
* [LLVM backend](docs/llvm.md)
* [Coyote Studio and the installer](docs/studio.md)
* [Debugging protocol](docs/debugging.md)
* [ML specification](docs/ml-spec.md)
* [Architecture](docs/architecture.md)
* [Brand assets](assets/brand/README.md)

## License

No license has been chosen yet; add a `LICENSE` file.

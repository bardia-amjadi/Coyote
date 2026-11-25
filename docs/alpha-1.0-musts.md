# What Coyote MUST have before 1.0 (analysis, 0.6.5.2)

Until 1.0 Coyote is **alpha**: syntax, libraries and the ML format may still change. 1.0 is the point where Coyote promises not to break your programs
(the same idea as the Rust 1.0 stability promise, see [rust-lang.org/2015/05/15/Rust-1.0](https://blog.Rust-lang.org/2015/05/15/Rust-1.0.html)). So the test for **MUST** is strict:

* **A. Contract**: if it is added or decided *after* 1.0 it would break existing programs, libraries or ML files, so it must be settled *before* the freeze.
* **B. Usable**: without it an ordinary program cannot be written, or needs an ugly workaround everybody would repeat.
* **C. Promise**: without it something Coyote already claims (native == PVM, runs on Windows, safe by default) is not true yet.

Everything that can be *added later without breaking anything* is **SHOULD** and goes to Gen One (1.x). Evidence below comes from probing 0.6.5.2 with small programs, not from guessing.

## MUST (in suggested order)

### A. Contract: decide and freeze

| # | MUST | Evidence today | Why it cannot wait |
|---|---|---|---|
| 1 | **Finish the "provisional" parts and publish a formal grammar + compatibility policy + ML policy** | README says the `method` syntax is "provisional"; 0.6.3 renamed half the keywords (`hidden`, `shared`, `here`, `field`, ...); ML went v5 to v10 in six releases | Renaming a keyword or an instruction after 1.0 breaks everyone. Write down: the grammar, what counts as a breaking change, how long an ML version is supported (the PVM should keep accepting old ML) |
| 2 | **Closure capture semantics** | a lambda captures the *value*; `n = n + 1` inside a lambda is a compile error ("keep changing state in an array or an object") | It changes what programs mean. Decide before 1.0 (recommended: capture the variable itself, using the same cell mechanism as `(ref)`) |
| 3 | **Top-level variables seen by methods (globals) or a clear "no globals" rule** | `var g = 1` at the top is *unknown* inside `method F()` (docs: "top-level globals: not implemented") | A rule every beginner hits on day one; it decides how entry files and libraries are laid out |
| 4 | **Typed exceptions + `finally` + stack traces** | `throw` only accepts a string (`throw new MyErr()` is an error); no `finally`; an uncaught error prints `file:line: runtime error: bad thing` with no call stack; host failures are plain strings | `throw`/`catch` syntax and the error type are public API: adding types later breaks every `catch (e)` that assumes a string. `finally` is needed for files/processes (resource safety) |
| 5 | **Object protocol: `ToString`, equality, hashing, ordering** | `"" + obj` is an error; `==` on objects/arrays is identity (`[1,2] == [1,2]` is false); map keys must be int/string/char/boolean/Guid; strings cannot be compared with `<` | Collections, maps, sorting, printing all depend on it; the protocol (which methods the language calls) is a contract |
| 6 | **Integer model: 64-bit `long`, `byte`, bitwise operators and shifts (`& | ^ ~ << >>`)** | `int` is 32-bit: `5000000000` is a compile error, `Time.UptimeMs()` overflows after 24.8 days, file sizes above 2 GB and Unix times after 2038 do not fit; no bitwise operators at all (the Encoding library had to use arithmetic) | New numeric types change library signatures (`UnixSeconds`, `File.Size`, `UptimeMs`): do it before the libraries are frozen |
| 7 | **Names and modules: qualified names, `use` with alias / selected names, nested namespaces** | docs: "not implemented: qualified names (`MyApp.Models.Point`), `use` with an alias or a list of names, namespaces that contain other namespaces" | Two libraries with a class named `Parser` cannot be used together. A package ecosystem needs this first |
| 8 | **Memory and value rules written down, including for native code** | the PVM has a GC; native code uses reference-counted strings and has no objects yet; `(copy)`/`(ref)` are new | "Everything native" needs one memory model (tracing GC in the native runtime, or ref-counting + cycle handling) and it must behave the same as the PVM, observably |

### B. Usable: ordinary programs must be writable

| # | MUST | Evidence today |
|---|---|---|
| 9 | **`enum` and `switch` / `match`** | `enum Color { Red }` is a syntax error; `switch` does not exist; libraries fake enums with shared objects (`DialogResult`) |
| 10 | **Bytes and binary I/O** (`array<byte>`, `File.ReadAllBytes/WriteAllBytes`, text <-> bytes in `Standard.Encoding`) | no way to read or write a non-text file (image, zip, any protocol); `Encoding` has no byte-array API |
| 11 | **Strings: indexing and iteration, comparison/ordering, and generic sorting** | `s[i]` and `in (var c of s)` are errors (`CharAt` only), `"a" < "b"` is an error; there is no `List.Sort` (only `Arrays.SortInt/SortDouble`), so strings and objects cannot be sorted |
| 12 | **A minimum standard library with one consistent style**: `List/Set/Stack/Queue` + `map` (exist), sort/min/max with a comparer, `DateTime` parts (year, month, ...; today only `Time.Format` strings), number/string formatting, file/dir/path/env/process (exist), HTTP client (exists) | the gaps are: sort with comparer, date parts, formatting. JSON and sockets are SHOULD (below) |

### C. Promise: what Coyote already claims must be true

| # | MUST | Evidence today |
|---|---|---|
| 13 | **Native really covers the language** (your priority): classes, arrays, maps, lambdas, `try`, host functions, mouths, the libraries; output identical to the PVM | native handles only int/boolean/string/console; everything else becomes a *bundled PVM executable*, which is not native. `tools/diff-native.sh` and the test suite compare PVM vs native and must keep passing (hotfix `X.Y.Z.9` first if not) |
| 14 | **A first real run on Windows**: CI build, tests, installer, Studio, VS Code extension, the TaskDialog `MessageBox`, DLL calls | nothing of this has ever run on real Windows or GitHub Actions; only Linux, MinGW cross-builds and Wine |
| 15 | **A conformance suite that covers every documented feature**, runs on the PVM and on native, on Linux and Windows, in CI | `tests/` has 12 cases + 4 error cases today; features such as the Windows libraries, Studio and the VS Code extension have none |

## SHOULD (Gen One, 1.x): additive, nothing breaks when they arrive

Ternary `?:`, `do ... while`, top-level `const`, default and named arguments, tuples / multiple return values, local (nested) functions, `abstract`
classes and methods, operator overloading (`operator` is reserved), generic constraints (`where T : I`), map literals, optional chaining `?.`, LINQ-style
`Where/Select` on arrays and lists, `async`/`await` and threads (decide the threading model early so it is not precluded, but it is not a 1.0 blocker),
compiled libraries (precompiled ML with headers; today `.lib` is a source bundle), a package registry and version resolution in `cpm` (today only `path` and `git`),
JSON, sockets, hashing/crypto, compression, a language server (the Studio has its own index), a doc generator, macOS and Linux installers, `dlopen` interop for Linux/macOS,
structural pattern matching beyond `switch`, a test runner command (`coyote test`; `Standard.Testing` exists), `Guid.New()` style helpers and other small library sugar.

## Suggested path (your call)

1. **0.7 Native**: item 13 with item 8 (memory model) and 15 (the suite compares both backends).
2. **0.8 Language freeze**: items 2, 3, 4, 5, 6, 7, 9 (they all touch the type system, the parser and ML), then item 1 (formal grammar, compatibility and ML policy).
3. **0.9 Library and verification**: items 10, 11, 12, then item 14 (everything on real Windows/CI), grow item 15.
4. **1.0-rc**: bug-fix only, no syntax or ML changes; 1.0 when item 14 and 15 are green.

Open question for you: items 4 and 5 are the biggest design choices (typed exceptions: a base `Exception` class with `message`? and what the object protocol is called). Everything else follows from conventions already in the language.

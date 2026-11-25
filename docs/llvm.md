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

* Types: `int` (i32), `long` (i64), `byte`, `double`, `char`, `boolean`, `string` (NUL-terminated bytes; constants containing NUL are rejected).
* `int` and `long` arithmetic with the PVM's checks (`integer overflow`, `long overflow`, `division by zero`, `remainder by zero`),
  bitwise and shift operators (counts taken modulo 32 / 64 like the PVM), comparisons, `double` arithmetic (IEEE, `%` is `fmod`),
  the conversions `int(x)`, `long(x)`, `double(x)`, `byte(x)`, `char(x)` with the PVM's range errors and messages,
  string `==`/`!=`/`<`/`<=`/`>`/`>=` (byte order = code point order), concatenation, `string(x)` for every scalar (doubles print exactly as on the PVM),
  `s.length` and `s[i]` counted in code points (UTF-8), `!`. (New in 0.9.0: everything except the `int`/`boolean`/`string` basics.)
* Arrays (0.9.1): `array<int|long|double|char|boolean|byte|string>` and arrays of arrays, as locals, parameters and results. A native heap with a precise,
  non-moving mark-and-sweep collector runs in the same places and with the same limit as the PVM (`out of memory: the heap limit of 8000000 array elements
  would be exceeded`, charged per slot; `array<byte>` is compact and charged 1 per 16 bytes). Generated code keeps every array-typed local and operand-stack
  slot in a per-function frame array (`rt_frame_push/pop`) which is the root set; a popped slot is cleared except in the allocating operations, which keep their
  operands rooted. An array of strings owns one string reference per element (released when the array is collected). Checked with valgrind (no invalid accesses).
* Classes (0.9.2): an instance is a heap object of 8-byte field slots whose class index selects a generated table (field kinds for the collector and for default values, names and the is-a matrix for `is`/`as`). Field access, `new`, casts, `Node?` (null = null pointer; `!` and null tests inline) and virtual-style calls (the compiler already lowers overriding to explicit type tests) are native, with the PVM's error messages (`null reference: cannot read field ...`, `invalid cast: A is not a B`, `cannot unwrap null`). Not yet: general `obj`/`dyn`.
* Closures and globals (0.9.4): a closure is a heap object reusing the class machinery: slot 0 holds a code pointer to a generated thunk `t.<function>(closure, declared args)` that loads the captured values (strings retained, shared cells are ordinary array references) and calls the function; slots 1.. are the captures, with GC kinds registered after the real classes in `rt_class_kinds`. `callfn` loads the pointer and calls indirectly, with the same 5000-call limit/trace and `null reference: cannot call a null function value`. File-level variables are LLVM globals `@g.N`; reference-typed ones are listed in `@rt_groots` and marked by `rt_collect`. Nullable `fn` types are still PVM-only.
* Maps and nullables (0.9.3): a map is a heap object (ek `m`) with an insertion-ordered entry table and a hash index; deletion leaves tombstones that are compacted when entries > 32 and dead > live, exactly like the PVM, so `keys()`/`values()` order matches. A nullable scalar or string (`int?`, `string?`) is a one-element box on the native heap (null = null pointer); nullable classes, arrays and maps are the pointer itself. Small accounting deviation: a box is charged one element against the GC trigger and 8,000,000 limit. Errors keep the PVM text: `key not found: <key>`, `null map reference`, `cannot convert "<t>" to int`, `cannot unwrap null`.
* Locals, parameters, calls (recursion included), `if`/`while`/`for`/`break`/`continue`/`return`.
* Host functions: only `con.print` and `con.println` (console). Runtime errors print
  `<file>.coy:<line>: runtime error: <message>` to stderr and exit 1, like the PVM.

## What the native (clang) path does not support (such programs are bundled instead)

`Guid`, arrays of other element types (maps, classes, nullable, function values), maps, nullable types, parsing (`int("12")`), `obj`/`dyn`, classes and globals, function values (lambdas), `try`/`throw`, any other
host function (including every `win.*`, `proc.*`, `rt.*` and `coy.*` function, so the Standard.Windows, Process, Runtime and interop libraries, and any program that uses the regular-expression engine or timers because those use classes and function values), and a backward jump to a label not reached by fall-through (the compiler never
generates one).

## Known differences from the PVM

Since 0.6.0.9 the native subset behaves like the PVM for everything it accepts: same output, same runtime errors (text, line and exit code 1), same limits.

* **Call depth.** Native code counts frames like the PVM (the entry function is frame 1). A call that would make more than 5000 nested
  calls is the runtime error `call stack overflow (more than 5000 nested calls; infinite recursion?)` at the call's line, instead of a crash.
  (Before 0.6.0.9 native code had no limit: 5100 nested calls succeeded and runaway recursion crashed with a segmentation fault.)
  On Windows the program is linked with a 16 MiB stack so that 5000 frames fit; that flag is **unverified on Windows**.
* **Strings are reference counted** (before 0.6.0.9 they were never freed). Each string on the operand stack or in a local owns one reference;
  constants are immortal; a string is freed when its last reference goes. A loop that builds 3 million strings peaks at the same memory as
  the PVM (about 13.5 MB). Checked with AddressSanitizer + LeakSanitizer (GCC) and valgrind: no leaks, no double frees.
* Host permissions are fixed at build time (console only); there is no `--allow`. Programs that use other host functions are bundled (they run on the PVM).
* Only verified on Linux with clang 18. Windows quoting of the clang command is untested.

Rule for every version: before delivering, run `tools/diff-native.sh <build-dir> <programs...>` on programs of the native subset that touch what
changed. It runs each program on the PVM and natively and compares stdout, stderr and the exit code. Programs outside the subset are reported as
`NOT NATIVE`: they run as bundles, which is the PVM itself, so they prove nothing about native code.

## How it is tested

In git history (before the tests were removed), `tests/cases/native/*.coy` were compiled to ML and run both by the PVM and natively; stdout, stderr and the
exit code must be identical, and stdout must equal a hand-written expected file (`native.*` in CTest, only
registered when `clang` is found). Unsupported programs are checked for their exact error text.
The tests were small (5 cases); there has been no fuzzing of this backend. `bundle.*` cases build bundled executables
(inheritance, generics, dead-code removal) and compare their output with the expected files.

## Stage 0.9 plan: "everything native" in slices (since 0.9.0)

Roadmap stage 0.9 makes every language feature native. It is too large for one delivery, so it is built and verified in slices; after each slice
`tools/diff-native.sh` must say SAME (stdout, stderr and exit code) for every program the slice newly accepts, and programs outside the native
subset keep building as bundled executables, so nothing that worked stops working.

| Slice | Content | State |
|---|---|---|
| 0.9.0 | scalar types: `long`, `byte`, `double`, `char`, bitwise operators, conversions, string indexing/length/ordering | **done** |
| 0.9.1 | native heap with a precise mark-and-sweep GC (frame arrays as roots); `array<T>` of int/long/double/char/boolean/byte/string and nested arrays: `new`, literals, `a[i]`, `a[i] = v`, `.length`, `.push`, `(copy)`, `==` on references, bounds/null/negative-size errors, the 8,000,000-element heap limit | **done** |
| 0.9.2 | classes: fields of scalar, string, array and class types, constructors, methods, inheritance and overriding, `is` / `as` casts, generics (the compiler monomorphizes them), `ToString` and `Equals` (they are ordinary methods); nullable class and array types (`Node?`, `!`, `== null`, `??` on references) | **done** |
| 0.9.3 | maps (`int`/`char`/`boolean`/`long`/`byte`/`string` keys, insertion order, compaction), `keys()`/`values()`, `tryGet`, nullable `int`/`long`/`double`/`string`/... (`?`, `!`, `??`), `int("..")`/`long`/`double` parsing with the PVM error messages | **done** |
| 0.9.3b | full `obj`/`dyn` (boxes of every type, `dyn` operators, `is`/`as` on obj); only the string/class boxes that exceptions need exist since 0.9.5 | planned |
| 0.9.4 | closures (`mkclosure`/`callfn`, shared cells, `fn(...)` values as parameters/fields/array elements), file-level variables (`getglobal`/`setglobal`) | **done** |
| 0.9.4b | the exception mouths (`sys.exception`, `sys.exceptionObject`, `sys.stackTrace`) came with 0.9.5; other mouths (`sys.exitCode`, ...) move to 0.9.6 with the host functions | partly done |
| 0.9.5 | `try/catch/finally`, `throw`/`throwx`, typed catch, rethrow, runtime errors as catchable exceptions, stack traces, the exception mouths and a minimal native `obj` (string and class boxes) | **done** |
| 0.9.6 | length-aware strings (NUL characters), backward jumps to loop heads at the start of a function, `ocopy`, host functions `text.*`, `math.*`, `time.uptimeMs/unixSeconds/sleep`, `con.readLine/hasInput`, `sys.exit/cwd/os`, `env.get/has`, `file.exists/readAll/writeAll/append`, `dir.exists`; the real host table is registered so every program verifies | **done (first part)** |
| 0.9.7 | `obj` boxes (int, long, byte, boolean, char, double, string, class instances) with `is`, unboxing, `==`/`!=` and `string(x)`; the mouths `sys.os/version/pid/exitCode` (exit code returned by the program); host functions `file.readBytes/writeBytes/appendBytes/size/delete`, `encoding.toBytes/fromBytes`, `time.format` | **done** |
| 0.9.8 | `dyn` operators (`+ - * / %`, comparisons, `==`/`!=`, unary `-`/`!`, same rules and error texts as the PVM), boxes of arrays and maps with scalar/string elements (`is array<T>`, unboxing by assignment, `d[i]` read and write through `dyn`) | **done** |
| 0.9.9 | `dyn` members/calls (`length`, `push`, `copy`, `containsKey`, `get`, `remove`, `keys`, `values`), host functions `random.*` (new portable generator, shared by PVM and native), `sys.arg*`/`sys.homeDir`, `file.copy/move/modified/fullPath`, `dir.create/delete/list`, `env.set`, `proc.pid/which`, `rt.*`; path checks (`the path is empty`) on all file hosts | **done** |
| 0.10 | still PVM-only: `proc.run/start/wait/kill/...`, `http`, `win`/clipboard/dialogs/registry, `coy.*`, boxes of nullables/nested arrays; hands-on Windows verification | planned |

Design of the heap slice (as built in 0.9.1): every heap object starts with a header (kind, element kind, mark bit, length, next); the generated code keeps every
reference-typed local and operand-stack slot in one per-function frame array that is linked into a global chain on entry and unlinked on return, so
the collector can find all roots precisely without scanning the C stack. Strings keep their reference counts (an array of strings owns one count per element).
The runtime stays plain C compiled together with the program by clang, as today.

## Exceptions (0.9.5)

`try` pushes a handler record (a 64-byte slot in the function: previous handler, an LLVM `sjlj` jump buffer, the GC frame chain and the call depth at that moment) and arms it with `llvm.eh.sjlj.setjmp`; `endtry` pops it. Every runtime error (`rt_fail_at` and friends), `throw` and `throwx` go through `rt_raise`, which builds the stack trace exactly as before, and then either prints the uncaught error and exits with code 1 (no handler) or restores `rt_frames` and `rt_depth` from the innermost handler and jumps to it with the message on the operand stack. `ret` restores the handler list of the function's entry, so `return` inside `try` needs no `endtry`.

Consequences worth knowing:
* In a function that contains a `try`, every access to a local is `volatile`; without that the optimizer could reuse values from before the jump. Functions without `try` are unaffected.
* Strings owned by the frames that an exception unwinds are not released (a small leak per thrown exception); arrays and objects are collected normally.
* Out-of-memory is not catchable, exactly as in the PVM.
* `obj` is only partly native: a string box (`rt_box_str`) or a class instance itself, which is all `Exception.Caught` needs; boxing any other type, `dyn` operators and `is` on obj keep a program on the PVM bundle until slice 0.9.3b.

Known gaps seen while checking the suite, planned for 0.9.6: backward jumps to a label not reached by fall-through (`basics`), `ocopy`, most host functions (`file`, `time`, `sys`, `regex`...) and the other mouths.

## Strings and host functions (0.9.6)

* **Strings are length-aware.** A string value points at its characters; the reference count sits just before them and the length in bytes before that (`{ i64 len, i64 count, chars..., 0 }`). A NUL byte also follows the characters so C functions still work, but nothing relies on it: concatenation, equality, ordering, map keys, `text.*`, parsing and printing all use the length, so strings with `char(0)` behave exactly as on the PVM (`Standard.Encoding` needs this). Error messages are still C strings and are cut at a NUL.
* **Permissions:** a native executable runs with every permission, like a bundled executable (`coyote run` grants only `console`, `time` and `random` by default). `tools/diff-native.sh` therefore runs the PVM with `--allow-all`.
* **Host functions:** `coyote-llvm` registers the real host table, so every program verifies; a call to a host function outside the native list above keeps the program on the PVM bundle. The error texts are the PVM's (`host function 'text.substring' failed: ...`). On Linux the executable is linked with `-lm`.
* `ocopy` (shallow copy of a class instance or a map) is native; a copied map keeps its entries in insertion order and drops dead entries.

## obj and mouths (0.9.7)

An `obj` is null, a class instance (the pointer itself) or a box: a heap object with one 8-byte slot and a type tag (`i l y b c d s`). Boxes are collected like any object (a boxed string is released with its box). `is` checks the tag (a class test is exact, like the PVM's), unboxing widens `int` to `long`/`double` like the PVM, and a failed conversion says `cannot convert <type of the value> to <type>`. Boxing an array, a map or a nullable value, and every `dyn` operator, still keep a program on the PVM bundle. `sys.exitCode` is stored in the runtime and becomes the process exit status when the program ends normally (an uncaught error still exits with 1, `sys.exit` with its argument).

## dyn and boxes of arrays/maps (0.9.8)

`dyn` values are the same obj boxes as in 0.9.7 and now take part in operators. `rt_dyn_bin` / `rt_dyn_un` copy the PVM's `dynBinary` / `dynUnary`: int+int stays int (overflow = "integer overflow"), anything with a long becomes long ("long overflow"), a double makes the result double, `string + x` concatenates, comparisons work on numbers, chars and strings, `==` compares numbers across kinds, other combinations raise `operator '+' cannot be applied to X and Y`.

An array or map whose elements (and keys) are scalars or strings can be boxed: a box with `ek 'x'`, `cap 'A'` (array) or `'M'` (map) and three slots {pointer, type name, element/key tags}. The GC marks the pointee of such a box. `is array<int>` compares the stored type name; `array<int> a = d` unboxes (error `cannot convert array<int> to array<string>`). `d[i]` / `d[i] = v` go through `rt_dyn_index` / `rt_dyn_setindex` with the PVM's checks and messages (`array<byte>` cannot be indexed through dyn). Still PVM-only: `dyn` member access and calls (`DynMember`/`DynInvoke`), boxes of nullables, nested arrays, `array<byte>` index, class arrays/maps. Test: `native_dyn`; `dyn_long` now runs natively.

## dyn members, more hosts, random (0.9.9)

`dyn` member access and calls (`rt_dyn_member`, `rt_dyn_invoke`) cover what the PVM covers: `length` of arrays, maps and strings; `push`/`copy` of arrays; `containsKey`/`get`/`remove`/`keys`/`values` of maps (`array<byte>` is rejected like on the PVM). Errors use the PVM's texts.

New native host functions: `sys.arg`/`sys.argCount` (the program's command-line arguments), `sys.homeDir`, `env.set`, `file.copy/move/modified/fullPath`, `dir.create/delete/list`, `proc.pid/which`, `rt.*`. Notes: error messages that carry the operating system's own text (`No such file or directory` ...) come from `strerror`; the PVM uses the C++ library's text, they matched on Linux, and may differ in wording on Windows. `file.fullPath` is `realpath` plus a lexical rest on Linux and `_fullpath` on Windows (the PVM uses `weakly_canonical`; symlink corner cases may differ). `rt.gcCollections/gcFreed/heapObjects/heapElements/memoryBytes` describe the native heap, so they differ from the PVM by nature. All file hosts now reject an empty path like the PVM did (the native versions did not before).

**Random numbers changed (PVM and native).** The generator is now xoshiro256** seeded through splitmix64 with Lemire's unbiased range reduction, written once in `src/pvm/host.cpp` (`Rng`) and once in the native runtime (`rt_rand_*`). A seeded program prints the same numbers on the PVM, in native code and on every platform (the old `std::mt19937_64` + `std::uniform_*` results were library specific). Programs that print seeded numbers get different numbers than in 0.9.8. Unseeded programs are random as before.

Still PVM-only (a bundled executable is built): `proc.run/start/stdout/...`, `http.*`, `win` hosts, `coy.*`.

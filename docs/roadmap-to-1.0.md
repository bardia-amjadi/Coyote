# Decisions and staged plan to 1.0 (review draft, 2026-11-07)

This turns the [MUST analysis](alpha-1.0-musts.md) and your 16 answers into decisions and a plan. **Nothing here is implemented yet**; it is for review before you decide.

## 1. Decisions taken (your answers)

| Topic | Decision |
|---|---|
| Exceptions | A base `Exception` class (message, trace); you subclass it; `catch (e: IoError)` picks by type; host failures become typed exceptions. `finally` is added. |
| Stack traces | An uncaught error prints `Error: message` plus `at Method (file.coy:line)` for every Coyote frame; the same text is in `Exception.trace`; native prints the same. |
| Closures | A lambda shares the variable it uses (`n = n + 1` inside a lambda works), built on the same cell mechanism as `(ref)`. |
| Top-level variables | File-level globals: top-level `var`/`locked` are visible in every method of the file (`public` ones to other files). |
| Object protocol | Well-known methods the language calls when present: `ToString()`, `Equals(other)`, `Hash()`, `CompareTo(other)`. They make `"" + obj`, `==`, class-typed map keys, `Set`, and sorting work. |
| Integers | Add `long` (64-bit) and `byte`; `int` stays 32-bit. Library times/sizes (`UnixSeconds`, `UptimeMs`, `File.Size`, ...) move to `long`. Bitwise operators `& | ^ ~ << >>` are added. |
| Bytes | A `byte` type and `array<byte>`; `File.ReadAllBytes/WriteAllBytes`, `Encoding.ToBytes/FromBytes`. |
| `enum` | Named constants with methods (`ToString`, parse, optional int values, comparable); no data per case. |
| `switch` | Both a statement and an expression form; exhaustive for enums. |
| Names/modules | Dotted qualified names (`MyApp.Models.Point`) and nested namespaces. **No** alias or selective `use` (you chose "dotted names only"). |
| Strings | `char` stays a Unicode code point; `s[i]` and `in (var c of s)` work; `Length()`/`CharAt` stay as aliases. Strings become comparable with `<` (code point order). |
| Native memory | A tracing GC in the native runtime, with the same behaviour as the PVM. |
| Order | Language freeze first, then native on the frozen language. |
| Concurrency | Shared-nothing tasks (own heap, talk by messages/channels), built in 1.x; the PVM/GC must not preclude it. |
| Compiled libraries, JSON | Gen One (1.x). |
| Breaking changes | Allowed until the freeze (with a `migrate-*.py` tool and notes); the freeze version is final. |
| Platforms | Windows and Linux first-class for 1.0; macOS in Gen One. |
| Windows testing | You test by hand at milestones, plus a GitHub Actions Windows test job. |

## 2. Staged plan (version numbers are yours to change)

| Stage | Scope | Why this order |
|---|---|---|
| **0.7 Core types and statements** | `long`, `byte`, `array<byte>`, bitwise operators, `char`/string indexing + iteration + comparison, `enum`, `switch` (statement and expression), library signatures moved to `long`, `File.ReadAllBytes/WriteAllBytes`. CI: add a Windows build + test job (the first real Windows signal). | mostly additive; changes library signatures, so it must come before the libraries are frozen |
| **0.8 Semantics + the freeze** (DONE in 0.8.0: nested namespaces were decided as qualified names only, see [language](language.md); `Hash`/`CompareTo` are reserved names whose use arrives with the 0.10 library) | `Exception` hierarchy + typed `catch` + `finally` + stack traces; shared closure variables; file-level globals; object protocol; dotted names + nested namespaces. At the end: the **language freeze**: formal grammar, compatibility policy, ML policy (which ML versions the PVM keeps accepting), keyword/syntax list declared final. | these change what existing programs mean (`catch (e)` gets an object, lambdas stop copying, `"" + obj` starts working); do them all in one migration |
| **0.9 Native on the frozen language** (IN PROGRESS: slice plan in [llvm.md](llvm.md); 0.9.0 = scalar types native) | a native runtime with a tracing GC; classes, arrays, maps, strings, lambdas, `try/finally`, exceptions with traces, host functions, mouths, the Standard/Community libraries; every conformance case equal on PVM and native, on Linux and Windows. Hotfix `0.9.x.9` first whenever native differs from the PVM. | built once, on a language that no longer moves |
| **0.10 Library minimum + verification** (IN PROGRESS: 0.10.0 = `List.Sort`/`Sorted` with a comparer, UTC calendar parts, number formatting, native job in `tests.yml`, [Windows checklist](windows-verification.md); still open: the naming pass, wider conformance coverage, your Windows pass) | generic sort with a comparer (`List.Sort`), date/time parts, number/string formatting, a consistent naming pass over the Standard libraries; conformance suite grown to cover every documented feature; your hands-on Windows pass over installer, Studio, VS Code extension, dialogs, DLL calls | the libraries rest on the finished language and native runtime |
| **1.0-rc, then 1.0** | bug fixes only; 1.0 when the Windows pass and the conformance suite are green on both backends | |

## 3. Risks I see (please read)

1. **Native is last under your order.** Until 0.9 every program with a class, array, map, lambda or `try` is still a bundled PVM executable. If you need real native earlier for a specific program, say so and I can pull a slice forward.
2. **`byte` / `array<byte>` needs compact storage.** PVM arrays hold full `Value` objects (tens of bytes per element); a 10 MB file would take hundreds of MB. This needs a dedicated typed-array representation (a PVM change, ML change, GC change), not just a type name.
3. **`catch (e)` changes meaning.** Today `e` is a string (`"caught " + e`). With an `Exception` class, `e` is an object; thanks to the `ToString()` protocol `"caught " + e` keeps working, but `e == "text"`, `throw "text"` and library code will need a migration script. `throw "text"` could stay as sugar for `throw new Exception("text")`: please decide.
4. **Shared closure variables + a native tracing GC** both depend on boxing cells; the GC must treat them as ordinary objects. Doing closures before native (as ordered) avoids rework.
5. **File-level globals need an initialization order** (file by file, top to bottom, libraries before the program). Cycles between files must be rejected. This also affects dead-code removal (an unused global with a side-effecting initializer).
6. **`long` doubles the numeric surface** (arithmetic, conversions, `Math`, formatting, native `i64`). Decide whether `int + long` widens implicitly (my default: yes, and `long -> int` needs an explicit conversion).
7. **Dotted names without alias** means a clash between two libraries that use the same full name has no escape hatch except renaming; acceptable for 1.0, but it should be revisited in Gen One.
8. **One person, one machine for Windows.** The Windows CI job and your manual passes are the only real Windows evidence; slipping them to the end (0.10) is a schedule risk, which is why 0.7 adds the CI job already.

## 4. Defaults I will use unless you say otherwise

* Method names are PascalCase as in the libraries (`ToString`, `Equals`, `Hash`, `CompareTo`); a class without them gets identity equality and the default `ToString` prints the class name.
* `int + long` widens to `long`; `long` to `int` needs `int(x)` and throws on overflow like today's checked arithmetic; literals get a `L` suffix (`5000000000L`) or become `long` automatically when they do not fit `int`.
* `enum` syntax: `enum Color { Red, Green = 5, Blue }`, used as `Color.Red`, `Color.Red.ToString()`, `Color.Parse("Red")`.
* `switch` expression: `var s = switch (c) { Color.Red => "r", Color.Green => "g", _ => "other" }`; statement: `switch (c) { case Color.Red: ... default: ... }`, no fall-through.
* The PVM keeps accepting every ML version from the freeze on, and drops ML versions older than the freeze version (alpha ML files are not promised to run after 1.0).
* Each stage ships as a version with docs, tests added to `tests/`, a `migrate-*.py` where something breaks, and a source tarball, as before.

## 5. Questions left for you

1. Should `throw "text"` stay as shorthand for `throw new Exception("text")` (easier migration) or be removed (cleaner)?
2. Keep the stage numbering 0.7 to 0.10, or use 0.7, 0.8, 0.9 and fold the library pass into them?
3. Does `Set`/`Hash()` need user-visible control now (for example `Equals` required whenever `Hash` is defined), or is "defined together or not at all" enough?

## 6. Gen One target (stated 2026-11-08)

**Started 2026-11-15/18 (0.11.0):** Vision core on Windows with native Win32 controls ([vision.md](vision.md)); first installer step = an optional "Vision UI" component. Still to do from this section: the new Visual-Studio-style installer (decided: small C++ Win32 bootstrapper, component manifest, optional download of Vision UI), Android, solutions with several projects, Studio templates/designer, compiled libs.

Gen One (1.x) aims at **Android and Windows application development** with a UI framework we write ourselves, **Vision UI** (library `CloudPixel.Vision.UI`). It needs:

* **A custom installer in the style of Visual Studio**: the user picks components; Vision UI is optional (download or not). When selected, the installer downloads and adds the Vision UI **project templates** and the library `CloudPixel.Vision.UI`.
* **Solutions with several projects** (a solution file grouping projects, shared build, references between projects), and a **better Studio UI** (project/solution explorer, templates dialog, designer for Vision UI).
* Consequences for the pre-1.0 work: the compiled-library format, the package registry/downloads in `cpm`, an Android target (native or PVM on Android), shared-nothing tasks for UI threads, and the installer's component model are all Gen One items; the 1.0 contract (language, ML, library layout, project/solution file schema) must leave room for them. The `.cyproj` schema is versioned (`schema v1`) for this reason.

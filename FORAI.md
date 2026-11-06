# FORAI.md - read this first (for the AI that continues this project)

Project: **Coyote**, a programming language + toolchain, written from scratch by Bardia (Iran). Current version: see `VERSION` (0.5.5).
Source files are `.coy` (the name is **Coyote**, never "Cayote"). Project files are `<Name>.cyproj`. Files start with `namespace Name`; `use "Name"` brings in a namespace of the project, an installed package or a `.lib` library (`import` was removed in 0.6).

## Rule about tests (important)

**This repository carries NO tests, on purpose.** The old suite (348 CTest cases, 19 Studio unit tests, 19 UI checks, about 3 MB)
made the project heavy and slowed the AI down. From now on, **in every version write only the tests relevant to that version's change**,
not the whole suite:

* Make a small test for the feature you are changing (one or a few cases), run it, report the result.
* Put it in `tests/` (CMake picks up `tests/CMakeLists.txt` when it exists, see the bottom of `CMakeLists.txt`) or run it ad hoc.
* Do not re-create or re-run "everything" every time. Do not add tests for untouched areas.
* Delete or trim the tests you added once the version is delivered, unless the user asks to keep them.
* The old suite is in git history if you need an example: `git show 97453e2:tests/CMakeLists.txt`, `git checkout 97453e2 -- tests studio/test editors/vscode/test` (check out into a scratch copy, not into the delivery).
  Test case styles used there: `tests/cases/ok/*.coy` + `.expected` (compile+run, stdout compared), `tests/cases/err/*` (diagnostics compared), cmake scripts in `tests/*_case.cmake`.

## Standing rules from the user

1. **Every delivery bumps `VERSION`** (then `node tools/version.js sync`; it writes `studio/package.json` and `editors/vscode/package.json`). CI builds the installer as `MAJOR.MINOR.PATCH.<run number>`.
2. **Every delivery ends with a source tarball** (`git archive --format=tar.gz --prefix=coyote-X.Y.Z/ -o ... HEAD`) sent with `SendUserFile`.
3. **Documentation is very important**: every feature gets documented in `docs/` (and the README status table). Do not describe features that do not exist (`docs/projects.md` states what is implemented).
4. **Honest reports**: say what was verified and what was not. Nothing has ever been run on real Windows or on GitHub Actions (llvm-mingw download, installer clang component, bundled executables on Windows, Studio on Windows are all unverified).
5. Do not touch the logo `assets/brand/cayote-logo-showcase.png` (the file name keeps the old spelling on purpose).
6. (No `.git` directory came with the 0.5.5 tarball: build the delivery tarball with `tar` if there is no repository.) Commit messages end with `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>` and the `Claude-Session: ...` line given in the session's system reminder.
7. An old memory note about "Cayote" (Cayote Lang, Kindows, Alpha Pack, 20 phases) is obsolete: this file and `docs/` are the truth.

## Layout

```
src/compiler  coyote: lexer, parser (error recovery via parseRecovering), sema (classes, inheritance, interfaces), generics.cpp (monomorphization), mlgen (ML text, dead-code removal), modules (namespace index + use resolution), libpack
src/pvm       pvm: ML loader/verifier, interpreter, GC, host registry, debugger (--debug), bundled-exe mode
src/llvm      coyote-llvm: native backend for a small subset + bundled-executable fallback (needs clang)
src/pkg       manifest (.cyproj / packages.json), resolver     src/cpx, src/cpm, src/cdt  (workflows, packages, check/format)
stdlib/       Standard.* libraries as .coy source (packed into standard.lib)   samples/  hello world etc.
studio/       Coyote Studio: Electron 33 + Monaco 0.52.2 (src/ main process, renderer/ UI)
editors/vscode  VS Code extension      installer/  Inno Setup script      .github/workflows/ci.yml  CI (not yet run on GitHub)
docs/         language, compiler, ml-spec, libraries, projects, cpx, cpm, cdt, llvm, debugging, studio, architecture
tools/version.js  version sync/check
```

Build: `cmake -S . -B build -DCOYOTE_BUILD_TESTS=OFF && cmake --build build -j8` (binaries: coyote, pvm, coyote-llvm, cpx, cpm, cdt).
Run a program: `build/coyote run hello.coy`. Studio: `cd studio && npm install && COYOTE_BIN=<build dir> npx electron .` (needs a display; `xvfb-run -a` on Linux; `unset NODE_OPTIONS`).

## Key design facts (so you do not rediscover them)

* **Pipeline:** `.coy` -> `coyote` -> textual **ML** (stack-machine text) -> `pvm`. ML version 5 normally; **ML 7** only when a class has supers (`class D extends A I`, `upcast`/`downcast`/`isinst`). See `docs/ml-spec.md`.
* **Inheritance:** base fields are flattened into the derived class; every instance method is virtual through compiler-synthesized dispatcher methods `Class.m$v` (chains of `isinst`, most derived first), so the PVM has no virtual call. Constructors of hierarchy classes are split into `$new` + `$init`. `override` is optional but validated. Source syntax: `class D : Base, I1`, `constructor(..) : base(args)`, `base.M()`, `x as T`, `x is T`.
* **Generics:** expanded before sema (monomorphization); explicit type arguments for generic methods (`First<int>(xs)`), no inference, no generic methods inside classes.
* **Dead-code removal:** reachability from top-level statements and static initializers; `--keep-unused` disables.
* **Bundled executable:** `[pvm exe][ML text][u64 LE length]["CYBNDL01"]`; `pvm` checks its own file at start. `coyote-llvm` uses it when a program is outside the native subset (unless `--native-only`).
* **Project file** `.cyproj` (JSON, schemaVersion 1): `name, version, type (app|library), entry, requires, output{directory,name,kind(native|ml),mode(auto|bundle|native-only),keepUnused}, run{args}, workflows`. `packages.json` is still read. cpx synthesizes `build` and `start` workflows.
* **Libraries:** `.lib` = source bundle; **compiled/linked libraries do not exist** (not started; generics must stay source). This was requested ("make libs from source code to real libs") and is still open.
* **Studio:** renderer scripts in order `core, themes, langindex, terminal, editor-lang, editor, views, projectview, run, settingsview, renderer`; global `App`; `window.__studio` is a test hook; IPC through `handle()` in `studio/src/main.js` and the `studio.*` bridge in `preload.js`. Theme = CSS variables on `<body>` + Monaco theme `coyote-live` (`renderer/themes.js`). The app window is `renderer/appwindow.html` (mirrors the Terminal panel; setting `appWindow`: debug|always|never).

## The motto: do not trust the PVM, worry about LLVM

The user considers native code more important than the VM. **In every version, native code must behave exactly like the PVM**, and if it does not, the next
version is a hotfix `X.Y.Z.9` (0.6.0.9 was one) that fixes it before any new feature. Run `tools/diff-native.sh <build-dir> <programs>` (see `docs/llvm.md`).
Remember that programs outside the native subset are bundled (PVM inside), so "works with LLVM" must not be claimed for them: say which programs ran natively.
`VERSION` may have a 4th part (hotfix); `tools/version.js` writes the first three into the package.json files and the installer uses `MAJOR.MINOR.PATCH.<run>`.

## Namespaces (0.6.0) - how they work, so you do not rediscover it

* The parser records `Program::ns` (`namespace A.B` must be the first declaration). `Module::ns` carries it into sema.
* `src/compiler/modules.cpp`: `use "X"` is resolved by `Loader::resolveImports`: a namespace of the project (an index built by scanning `.coy` files under the project root = nearest folder with a manifest, reading only each file's first declaration), an installed package, or a library; more than one kind = "ambiguous" error. Only the files that are `use`d (and same-namespace neighbours) are loaded.
* Same-namespace files see each other's exports (the loader adds them to `importIndex`, like `libSiblings`); their top-level statements are ignored (`add(..., ignoreStatements)`), so a second script in the same namespace does not break the entry file.
* **Sema is unchanged**: it still works per module with `importIndex` (exports of the used modules). Visibility = `export`; there are no qualified names.
* Studio (`renderer/editor.js` `langHost`, `editor-lang.js`, `langindex.js`): `ix.namespace`, `use "` completion lists namespaces + libs, `namespace ` completion suggests `<Project>.<Folder>`, picking a symbol of another namespace adds `use "<ns>"`. **Only syntax-checked and unit-tested at the language-index level, never run in Electron.**
* Parser recovery: `Parser::run(errors)` catches per top-level item and per block statement (`recoverItem`/`recoverStatement`), max 50 errors, one per line. `parse()` still throws on the first error (used by cdt).
* Delivery note: `tools/stage.sh` copies `samples/*` with plain `cp`, so do not add sub-folders to `samples/` without changing it.

## Status and what is left

Done in 0.6.0.9 (hotfix, native backend): same 5000-call depth limit as the PVM, reference-counted strings (no more leaks), Windows link stack flag (unverified), `tools/diff-native.sh`.

Done in 0.6.0: namespaces + `use` for local namespaces (replaces `import`), Studio namespace suggestions, parser error recovery, `++`/`--`, compound assignment on array elements, `$"..{x}.."` interpolated strings, docs cleanup (language.md header/model example/method parameter types/reserved words, README ML versions and verification section), clearer `coyote-llvm` fallback note, the CI workflow file replaced by the user's version (Inno Setup located instead of installed with choco).

The user's 0.6 selection was staged like this (agreed): **0.6.0** (done, above, except the items below) -> **0.6.0.9** (done) -> **0.6.1** overloading + generic inference + remaining polish (top-level globals, `else` without braces, map literals, iterating strings/maps) -> **0.6.2** lambdas/closures + try/catch -> **0.6.3** `Standard.Windows` (new low-level `hand` modules first, only what is needed, then the library on top: MessageBox, file dialogs, clipboard, registry/processes, windows/notifications; written to compile and be unit-checked on Linux, **cannot be run without Windows**) -> **0.6.4** remove every duplicate class/method in the Standard libraries, `ref`/`copy` value semantics (`var st = (ref)s`, `var st = (copy)s`; plain `var st = s` keeps today's behavior), better testing -> then the user decides whether to go to **0.7** (compiled libraries, native backend coverage, Studio/VS Code features (go-to-definition for inherited members, rename, find references, language server)).

While doing each stage, check that the native backend still matches the PVM (and extend its subset when the new feature is something native should cover).

Done in 0.6.1: overloading (`mangleOverloads` + `pickOverload` in sema.cpp, names `Name$$<sig>`), generic inference (light typer in generics.cpp, `inferTypeArgs`), braceless bodies (`bodyOrStatement`), `for (k in map)` (sema wraps `.keys()`). Deferred (still open): top-level globals, map literals, iterating strings, Studio index awareness of overloads.

Done in 0.6.2: lambdas/closures (Type::Fn with params in `targs` and result in `elem`; `checkLambda` in sema.cpp lifts the body into a `lifted` method `$lambdaN` whose first parameters are the captured variables and `this`, node becomes `mkclosure`; `CallFn` for `f(x)`, `make()(x)`, `obj.field(x)`; method references via `tryFunctionRef`; untyped parameters wait in `unresolvedLambdas_` until `checkArg`/`checkValue` give an expected type) and try/catch/throw (ML 8 `try`/`endtry`/`throw`; `Frame::handlers` in the interpreter; mlgen emits `endtry` before `break`/`continue` out of a `try`). Deferred: `finally`, exception objects with types/stack traces, assignment to captured variables, native (LLVM) support for both, Studio index awareness (hover for lambdas).

Why overloading/inference were not in 0.6.0: method tables are keyed by name in sema (module tables, `ClassInfo::methods`, dispatchers `(class,name)`, override checks), and generics are expanded *before* sema so call-site argument types are unknown there. Plan: mangle overloaded declarations (`name$sig`), keep an overload set per name, pick by argument types in `checkCall`/method-call resolution, and keep mangling consistent across a class hierarchy (an override must keep the same mangled name). For inference, either infer in sema and expand lazily, or restrict the pre-sema pass to literal/`new`/declared-variable arguments.

Done in 0.6.3: keyword changes (`private`->`hidden`, `static`->`shared`, `this`->`here`, `export`->`public`, foreach `in (var x of xs)`; old words are reserved and `failIfRenamed` in parser.cpp gives the rename message; the internal Var is still called "this"), required `field`/`property`, `locked` (constant) and `readonly` (initializer or declaring-class constructor; `Stmt::fieldInit`), `shared class`, auto properties with initializers, classes/`use` in the entry file, `new T().M()` in expressions, and `Standard.Windows` (host module `win` in `src/pvm/host_win.cpp`, permission `windows`, classes in `stdlib/Standard.Windows`). All `.coy` files were migrated with `tools/migrate-0.6.3.py` (kept for people with old code; it handles fields/keywords but not inline class bodies on one line). Not verified: GUI dialogs, real Windows, Electron. Deferred to 0.6.4: removing duplicate classes/methods in the Standard libraries, `(ref)`/`(copy)` value semantics, better testing.

## How to work in this repo efficiently

* Read `docs/language.md`, `docs/projects.md`, `docs/compiler.md` before changing the language or toolchain; keep them updated.
* Prefer small changes + one focused test over broad test runs.
* Keep outputs honest: list what was verified (and how) and what was not.

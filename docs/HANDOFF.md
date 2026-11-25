# Coyote: handoff for the next chat / AI (state at 0.11.0, 2026-11-18; sections 1-6 were written at 0.10.2)

> **0.11.0 (Gen One start): Vision UI core is in** - read `docs/vision.md` and the 0.11.0 note at the end of `FORAI.md`. The owner said "we will test (Windows) later, now Gen One". Next in Gen One: the new Visual-Studio-style installer (decided: small C++ Win32 bootstrapper reading a component manifest, Vision UI optional/downloadable), then more controls/Android/solutions. The Windows selftest (now with a Vision step) is still the first thing to run when he comes back.

Read this first, then `FORAI.md` (per-version technical notes, newest at the bottom), `docs/roadmap-to-1.0.md`, `docs/llvm.md` (native backend)
and `docs/windows-verification.md`. The owner is **Bardia Amjadi**; he gives short messages ("next", "continue") and expects you to proceed.

## 1. What Coyote is
Own programming language + toolchain, all C++17: compiler (`coyote`) -> textual ML -> PVM (`pvm`, the reference interpreter); native backend
`coyote-llvm` (ML -> LLVM IR + a C runtime -> clang); package/project tools `cpx` (projects, `.cyproj`), `cpm`, `cdt`; Coyote Studio (Electron
IDE); VS Code extension; Inno Setup installer; GitHub Actions (`.github/workflows/ci.yml` is **user-supplied: never touch it**; `tests.yml` is ours).
Language: source `.coy`; project `.cyproj`; name is spelled **Coyote** (never "Cayote"). Standard libs in `stdlib/` (Coyote source, compiled into the build).

## 2. Standing rules from the owner
* Every delivery: bump `VERSION`, run `node tools/version.js sync`, document every feature (`docs/`, `README.md`, `FORAI.md` note, `docs/testing.md`), and **end with a source tarball**
  (copy the tree to `coyote-X.Y.Z`, `tar --exclude=.git --exclude=build --exclude='b*' (build dirs) --exclude=node_modules`, send with SendUserFile).
* Be honest about what was and was not verified. **Nothing has ever been run on real Windows or on GitHub Actions; Studio and the VS Code extension were never run.**
* Do not touch `assets/brand/cayote-logo-showcase.png`. Commit messages (if git is used) end with the attribution lines the system gives.
* **Motto: "don't trust the PVM, worry about LLVM".** Native must behave exactly like the PVM (stdout, stderr, exit code). If native differs: hotfix `X.Y.Z.9` first.
* Priority is **NATIVE** ("everything native": real native code, not the bundled-PVM fallback). Main target afterwards: **Gen One (1.x)** = Android + Windows apps with the owner's
  own UI framework **Vision UI** (library `CloudPixel.Vision.UI`), a Visual-Studio-style installer with optional components (Vision UI optional download, project templates), solutions with several projects, better Studio UI. Compiled libs and JSON are Gen One. Windows + Linux are first-class for 1.0; Windows is tested by him + Actions.
* Language is **frozen** (0.8): no breaking language changes without his decision. Library renames are a deliberate break: list them and ask first.
* He wants MUST (before 1.0) strictly separated from SHOULD (Gen One).

## 3. Where things stand
Versions 0.9.0-0.9.9 built the native backend slice by slice (scalars, heap+GC, arrays, classes, maps, closures, globals, exceptions via `llvm.eh.sjlj`, strings, obj/dyn boxes,
hosts). 0.10.0 added `List.Sort`/`Sorted(comparer)`, UTC calendar parts (`Time.FromParts/Year/...`), `Numbers.ToBase/Hex/Binary/Thousands/Fixed`, a native job in `tests.yml`.
0.10.1 added **`selftest/`** (verification apps + one-click runner) and fixed `Process.RunFull` stdin. 0.10.2 = this handoff only.
Linux evidence: suite 34 programs on PVM, `--native` (all 34 identical), `--gc-stress`; valgrind clean on the native tests; fresh-extract build passes.

**The very next step: the owner runs `selftest\run-selftest.bat` on Windows and sends back `selftest-report.txt`.** Fix what it shows (native != PVM => hotfix `X.Y.Z.9`).
If a Windows-only part of the tests (registry, clipboard, DLL calls, dialogs) fails, it may be the test (written from the library docs, never run) rather than the library.

## 4. Remaining work (in order)
1. Process the Windows report (above). Riskiest area: native exceptions (`llvm.eh.sjlj` = setjmp/longjmp, never run on the Windows C runtime), native stack depth (limit is 5000 nested calls, same as the PVM), argv encoding (Windows narrow `argv` is the ANSI code page, not UTF-8: native `Environment.Arg` of non-ASCII text probably garbled; fix = `GetCommandLineW` + convert), CRLF in stdout, `file.fullPath`/`rename` semantics.
2. Rest of 0.10: library **naming pass** (nothing renamed yet; propose list, get approval, ship a migration script), wider conformance coverage, remove `continue-on-error` from the two native steps in `tests.yml` once green on Windows.
3. Native for the PVM-only hosts: `proc.run/start/wait/kill/...`, `http.*`, `win.*` (registry, clipboard, dialogs, windows, notifications), `coy.*`. Programs using them currently build as bundled executables (`coyote llvm` prints `note: host function 'x' is not available in native code yet`). Needs Windows API code that cannot be run in the Linux container: write it carefully and let the owner's selftest verify.
4. `dyn`/`obj` gaps in native: boxes of nullables, nested arrays, class arrays/maps. `dyn` member access covers only built-in members (`length push copy containsKey get remove keys values`).
5. Known small gaps: `Upper()/Lower()` ASCII only; OS error texts may differ per OS (native `strerror` vs PVM C++ library); `rt.gc*`/`rt.heap*` differ between PVM and native by nature.
6. Then 1.0-rc (bug fixes only) -> 1.0 -> Gen One.

## 5. Build and test (Linux container)
```
cmake -S coyote-X.Y.Z -B b && make -C b -j8                       # tools: coyote pvm cpx cpm cdt coyote-llvm (needs clang for native)
bash tests/run.sh b                  # PVM suite      (expects "N passed, 0 failed")
bash tests/run.sh b --native         # native vs PVM  (tools/diff-native.sh does SAME / DIFFERENT / NOT NATIVE)
bash tests/run.sh b --gc-stress      # run SEPARATELY: it is slow (tool time limit is 2 minutes)
coyote run selftest/Runner/src/main.coy -- --bin b --quick       # the full verification, writes selftest-report.txt (stop after ~90 s: run in background)
```
Stdlib sources are compiled into the build: after editing `stdlib/` rebuild before testing. Test programs: `tests/cases/*.coy` + `.out` (blessed with `--bless`, never bless without checking), `.in` stdin, `.rc` expected exit code.
MinGW syntax check of the native runtime (the only Windows-side check possible here): extract the `R"RT(...)RT"` C text from `src/llvm/main.cpp` to a file and
`clang --target=x86_64-w64-mingw32 -fsyntax-only -Werror=implicit-function-declaration -Wno-everything runtime.c` (do not use `-Wno-everything` alone: it hides missing functions like `lstat`).

## 6. Native backend essentials (`src/llvm/main.cpp`, ~3000 lines: C runtime in a raw string + LLVM IR emitter)
* Slot kinds `i b s l y d c r`; strings refcounted and length-aware (header {len,count}, chars at +16); everything else is `RtObj` (header offsets next0 mark8 ek12 len16 cap24 weight32 data40) under a precise mark-sweep GC with shadow frames `rt_frames`.
  `ek`: 'i' 'l' 'd' 's' 'r' 'y'(bytes) 'o'(class, cap = class index) 'm'(map) 'x'(obj box: cap = tag `i l y b c d s`, or 'A'/'M' for array/map boxes holding {pointer,type name,tags}).
* Rooting rule: operands stay in stack slots (`peekk`) during a runtime call that can allocate, then `clearSlot` + pop; a fresh object that is not yet on the stack must be put in `rt_tmp_root` before the next allocation.
* Exceptions: `llvm.eh.sjlj`; caller must store `llvm.frameaddress(0)` in jb[0] and `llvm.stacksave` in jb[2]; handler alloca `[64 x i8]`; volatile locals in try functions; `rt_raise` is the single error exit; unwound frames' strings leak.
* Host functions: C functions `rt_h_*` registered in the `nat` map in the `HostCall` case (`{"file.copy", {"rt_h_file_copy", true /*gets fn,file,line*/, -1}}`) + `declare` lines in the IR header. A program using any host not in `nat` is "not native" and becomes a bundle. All path hosts call `rt_pchk` (empty path = error, like the PVM).
* The random generator exists twice and must stay identical: `Rng` in `src/pvm/host.cpp` and `rt_rand_*` in the native runtime (xoshiro256** + splitmix64 + Lemire).
* Native executables have all permissions; `diff-native` runs the PVM with `--allow-all`.

## 7. Language/library gotchas learned the hard way
No `?:` operator; array type is `array<T>` (no `T[]` in declarations); block lambdas need `: type` after the parameter list (`(int a, int b) : int => { ... }`); `hand:` declarations are file-scoped,
bare names (two libraries' `file exists` and `dir exists` cannot share a file); library `model` types (ProcessInfo, WindowInfo) cannot be named: use `var`; `Process.Run` throws for a missing program;
`File.Delete`/`Directory.Delete` return false for missing; `try { } catch (e) { e.Message }`; class unbox from `dyn`/`obj` is by assignment (`array<int> a = d`) or `is`; loops are `for (var i = 0; ...)` and `in (var x of xs)`.
Tool limits in the container: a foreground command is killed after 2 minutes (run long things in the background and poll); `rm -rf` of the work dirs before copying trees; the dev tree was `coyote/coyote-0.9.0` and build dir `b90` (session-local, gone in a new chat: start from the tarball).

## 8. Memory
The owner's persistent memory file `/areas/coyote-lang.md` holds the decision log (read it with the memory tools; append one line per delivery).

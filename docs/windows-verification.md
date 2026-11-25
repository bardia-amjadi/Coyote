# Windows verification (0.10)

Coyote is developed in a Linux container; nothing has been run on Windows by the author of the code. The only Windows evidence is the
GitHub Actions run (`tests.yml`) and a MinGW *compile* check of the native runtime. This page is how you produce real evidence in a few
minutes, and how to read it.

## The one-click run

1. Install Coyote (the installer), or build it; open the unpacked source folder (it contains `tests\` and `selftest\`).
2. Double-click `selftest\run-selftest.bat` (or run it in a terminal). Options: `--bin "C:\Program Files\Coyote\bin"` (when `coyote.exe` is not
   on the PATH), `--quick` (skip the slow GC-stress pass), `--ui` (also the interactive part).
3. It takes a few minutes and writes **`selftest-report.txt`** next to the `tests` folder. **Send that file back.** When something failed, also
   zip the folder `selftest-work` (the logs of every run).

The runner is itself a Coyote program (`selftest\Runner`), so it also uses Process, File and Directory; if it cannot even start, that is the first finding.

## What it runs

| Step | What it checks |
|---|---|
| environment | Windows version, code page, tool versions, clang found next to Coyote |
| conformance, PVM | every program of `tests\cases` against its blessed output and exit code, plus the multi-file project and the expected compile errors |
| GC stress | the same programs with the collector forced before every allocation |
| native vs PVM | the same programs as real native executables: stdout, stderr and exit code must equal the PVM's (line-ending-only differences are reported separately) |
| SelfTest (core) | ~100 checks run as PVM *and* as a native executable: files (spaces, Unicode, 1 MB files, copy/move/replace), environment, time, math, text, encoding, collections, exceptions in loops, 300000 objects, 100000-entry maps; then recursion 4900 deep (with and without try/finally in every frame), endless recursion = clean error, command-line arguments with spaces/quotes/Unicode |
| SelfTestSystem | processes (run, stdin, stdout/stderr, exit codes, timeout, start/kill, working folder), registry, clipboard, process list, calling Windows DLL functions, HTTP/HTTPS, loading Coyote programs |
| project flow | `cpx check`, `cpx build`, and running the executable `cpx` built |
| interactive (`--ui`) | message boxes (modern style, icons, every button set), long and Unicode text, open/save/folder dialogs, notification, clipboard paste, opening Notepad; you answer "did it look right?" |

Report lines: `ok` / `FAIL` / `--` (the program is outside the native subset: runs as a bundled executable) / `NOTE`. `PASS`/`FAIL`/`SKIP` lines come from the
self-test apps; `INFO` lines are machine details.

## Known gaps that are not bugs

* `proc`, `http`, `win` and `coy` hosts are PVM-only: programs using them build as bundled executables (the SelfTestSystem project is one). The report shows `--` for them.
* Operating-system error texts can be worded differently on Windows (`strerror` in native code, the C++ library in the PVM); the tests print only "caught" for those.
* `Upper()`/`Lower()` change only ASCII letters.

## Manual checklist (not covered by the runner)

- [ ] Installer: install, choose components, uninstall; a new terminal finds `coyote`, `pvm`, `cpx`, `cpm`, `cdt`, `coyote-llvm`.
- [ ] Coyote Studio: open a project, run, debug (breakpoint, step), stop; the output panel shows the program output.
- [ ] VS Code extension: install the `.vsix`; highlighting, diagnostics, run.
- [ ] A program that uses `readLine` in a console window.

Anything where the native executable differs from the PVM becomes a `0.10.x.9` hotfix (motto: don't trust the PVM, worry about LLVM).


## Vision UI (0.11.0)
Needs the "Vision UI" installer component (or `visionui.lib` in `libs\`). `run-selftest.bat` runs `selftest\SelfTestVision` automatically (a window flashes on screen; ~15 checks:
real Win32 backend, client size round trip at your DPI, text read back from the native controls, layout bounds, click/type/check events, enabled state) and, with `--ui`, a 4-second look-and-answer step.
By hand: `cpm init VisionTest --vision` in an empty folder, then `coyote run src\main.coy`: expect a window "VisionTest" with a Material look, a flat purple "Click Me" button; each click increases the counter text
that appears after the first click; closing the window ends the program. Report anything that looks wrong (clipped text, wrong colours, a button that does not react, a window that stays open after closing, blurry text on a high-DPI screen).

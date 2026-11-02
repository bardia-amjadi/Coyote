# Coyote Studio and the Windows installer

**Coyote Studio** is a desktop IDE (Electron + Monaco) that uses the same toolchain as the command line:
`coyote` (diagnostics, build, run), `cdt` (format), `cpx` (project workflows), `cpm` (packages), `coyote-llvm` (native executables) and `pvm --debug` ([debugging](debugging.md)).

Libraries: New Project asks for `app` or `library`; Tools > Build Library packs a library project to `out/<name>.lib`, and Build and Install Library for Me also copies it to `~/coyote/libs` so any program can `use` it ([libraries](libraries.md)). The installer puts the Standard library in `<install>\libs`.

Layout (like Visual Studio / VS Code): activity bar (Explorer, Find in Files, Run and Debug, Libraries), editor tabs with breadcrumbs, a bottom panel (Terminal, Output, Problems, Generated ML), a status bar and a command palette (`Ctrl+Shift+P`), Go to File (`Ctrl+P`), Go to Symbol (`Ctrl+Shift+O`, `Ctrl+T` for the whole project and libraries). Dark and light themes, settings dialog, new-project dialog (Console App or Library).

**Terminal.** Programs run in the Terminal panel. Whatever you type there is sent to the program, so `Shell.ReadLine()` / `Shell.Prompt("Name: ")` work (also while debugging). Enter sends a line, `Ctrl+D` (or the End input button) ends the input, `Ctrl+C` stops the program, Up/Down recall earlier lines. A prompt printed without a line break stays on the same line as your input.

**Language service.** Completion for keywords, locals, methods, classes and members after a dot (arrays and maps included), with the documentation written as `#` comments in the source. Picking a symbol from a library you have not `use`d yet **adds the `use "<Library>"` line for you** (the same for exported symbols of other files: it adds an `import`). The quick fix (`Ctrl+.`) on "unknown variable/class" does the same. Hover, signature help, Go to Definition (`F12`, library sources open read-only), outline, semantic highlighting (classes, methods, parameters, properties get their own colors), format document, and diagnostics as you type (the unsaved text is checked).

**Libraries view.** Lists every installed `.lib` with its classes and members; click to read the source.

Features: file tree, find in files, run file / run project, a debugger (breakpoints, step over/into/out, call stack, variables, globals, program input), "show ML", native build, library build and install, and a toolchain doctor.
Opening a `.coy` file from Explorer (or `CoyoteStudio.exe file.coy`) opens it in the running Studio.

The toolbar logo is a downscaled copy; the original in `assets/brand/` is untouched and shipped with the installer.

## Installer (`Coyote-Setup-<version>.exe`)

Built by the `windows-installer` CI job with Inno Setup. Components: **Coyote toolchain** (always), **Coyote Studio** (optional) and the **VS Code extension** (optional). The "Only Coyote" type installs just the command-line tools. Optional tasks: add to `PATH`, desktop icon, `.coy` association. It installs per user or for all users, needs no VC++ redistributable (static CRT) and has an uninstaller that also removes the PATH entry.

### Update behaviour

* Newer setup over an older install: the wizard says **Update**, skips the folder/components/tasks pages, keeps your previous choices and replaces the programs.
* Same version: **Repair**.
* Older setup over a newer install: refused with an explanation. Uninstall first to downgrade.

### Versioning rule

`VERSION` holds `MAJOR.MINOR.PATCH`. The installer version is `MAJOR.MINOR.PATCH.<CI run number>`, so **every CI build is newer than the previous one** even when `VERSION` did not change. **Bump `VERSION` for every delivery** (`node tools/version.js sync` writes it into the Studio and VS Code manifests; `check` is run in CI).

## What is verified

On Linux: the Studio unit tests, an end-to-end UI test under Electron (opening a file from the command line, diagnostics, run, debugger, show ML), `electron-packager` producing the Windows app directory, and the installer script **compiled and exercised under Wine with a 32-bit test build**: fresh install, update, same-version repair, downgrade refusal, cli-only type, uninstall and PATH cleanup.
The installed `coyote.exe` (Wine, 32-bit test build) found `libs\standard.lib` and ran `use "Standard"` programs, including after an update.
**Not verified:** the real `windows-2022` CI job, an MSVC build, the packaged `CoyoteStudio.exe` actually running on Windows, and the installer's UI (only silent mode was exercised).

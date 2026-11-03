# Coyote Studio and the Windows installer

**Coyote Studio** is a desktop IDE (Electron + Monaco) that uses the same toolchain as the command line:
`coyote` (diagnostics, build, run), `cdt` (format), `cpx` (project workflows), `cpm` (packages), `coyote-llvm` (native executables) and `pvm --debug` ([debugging](debugging.md)).

## Projects, building and running

A Studio project is a folder with a project file, `<Name>.cyproj` (for example `MyApp.cyproj`; the format is in [projects](projects.md)). **File > New Project** creates the folder, the project file and a starter program; **File > Open Project...** opens a folder through its `.cyproj`; for a folder that has none, the Project Manager offers **Create project file here**.

**Project Manager** (activity bar, `Ctrl+Shift+P` is the palette, use *View: Project Manager*): shows and edits the project file: name, version, type (app or library), description, entry point, **output** (folder, name, kind *native executable* or *ML file*, mode *auto / bundle / native only*, keep unused code) and the arguments passed when running. Nothing about the output is chosen with buttons: the settings live in the `.cyproj` file, so the command line (`cpx build`) and Studio always agree. It also lists the project's `.coy` files (right click: **Set as entry point**).

**Build** (toolbar, `F7`), **Rebuild** (`Ctrl+Shift+F7`), **Clean** and **Open output folder** (Build menu, Project Manager) run the project's `build` workflow into the Output panel and write the file the project file names. Library projects build `out/<name>.lib`; *Build and Install for Me* also copies it to `~/coyote/libs` ([libraries](libraries.md)). **Run** (`F5`) runs the current file or, with the toolbar target set to *Project*, the entry point with the configured arguments.

**Native builds** need clang. The Windows installer includes it (component *Native builds*), `coyote-llvm` finds it next to itself, and paths with spaces work. Without clang, `mode auto` still produces an executable by bundling the program with the PVM ([llvm](llvm.md)).

## The app window

When you **debug** (default), Studio opens the program in its own window: a separate, modern console that shows the program's output and takes its input (`ReadLine` works there; Enter sends a line, `Ctrl+D` or *End input* ends the input, `Ctrl+C` or *Stop* stops the program). The Terminal panel mirrors everything, so you can also type there. Closing the window stops the program. Settings > *Terminal and app window* > *Show the running program in its own window*: **When debugging** (default), **Run and debug**, or **Never**.

## Settings and themes

*Preferences: Settings* opens a full page with categories and a search box; every change applies at once.

* **Appearance:** six themes (Coyote Dark, Coyote Light, Midnight, Graphite, Solarized Dark, High contrast), an accent color, interface font size, density.
* **Colors:** every color of the window (backgrounds, text, borders, status bar, error/success/warning...) can be overridden; *Reset* returns one to the theme's value.
* **Syntax colors:** the colors of comments, keywords, control flow, types, methods, variables, properties, strings, numbers, operators... with a live preview.
* **Editor:** font, ligatures, line height, tab size, word wrap, minimap, line numbers, whitespace, cursor, bracket colors, indent guides, sticky scroll, format on save, auto save.
* **Suggestions and checks:** error checking while typing, suggestions while typing, Enter to accept, parameter hints, and suggesting classes of libraries you have not `use`d yet.
* **Terminal and app window**, **Build and run**, **Toolchain** (folder, doctor, reset all).

Settings are stored in `settings.json` in Studio's user-data folder.

## Layout and editing

Layout (like Visual Studio / VS Code): activity bar (Explorer, Find in Files, Run and Debug, Libraries, Project Manager), editor tabs with breadcrumbs, a bottom panel (Terminal, Output, Problems, Generated ML), a status bar and a command palette (`Ctrl+Shift+P`), Go to File (`Ctrl+P`), Go to Symbol (`Ctrl+Shift+O`, `Ctrl+T` for the whole project and libraries).

**Terminal.** Programs run in the Terminal panel (and the app window). Whatever you type there is sent to the program, so `Shell.ReadLine()` / `Shell.Prompt("Name: ")` work (also while debugging). Enter sends a line, `Ctrl+D` (or the End input button) ends the input, `Ctrl+C` stops the program, Up/Down recall earlier lines. A prompt printed without a line break stays on the same line as your input.

**Language service.** Highlighting knows every keyword (`interface`, `protected`, `override`, `base`, `as`, `is`, generics `<T>`, class headers `class D : Base, I`). Completion covers keywords, locals, methods, classes and members after a dot (arrays and maps included, **inherited members and generic type arguments** too: `Box<int>.Get()` is an `int`), with the documentation written as `#` comments in the source. Context-aware suggestions:

* `use "` lists the installed libraries (with their classes); `import "` lists the project's source files and what they export.
* `class X : ` and `interface X : ` list the classes and interfaces you can extend (including ones from libraries you have not `use`d: choosing one adds the `use` line).
* `override ` lists the inherited methods you can override and writes the whole declaration; `base.` lists the base class's members.
* `x as `, `x is ` and `new ` list classes and types; `new` of a generic class inserts `<T>`.
* Snippets: `classinherit`, `interface`, `generic class`, `generic method`, `override`.

Picking a symbol from a library you have not `use`d yet **adds the `use "<Library>"` line for you** (the same for exported symbols of other files: it adds an `import`). The quick fix (`Ctrl+.`) on "unknown variable/class" does the same. Hover, signature help, Go to Definition (`F12`, library sources open read-only), outline, semantic highlighting, format document, and diagnostics as you type (the unsaved text is checked).

**Libraries view.** Lists every installed `.lib` with its classes and members; click to read the source.

Features: file tree, find in files, run file / run project, a debugger (breakpoints, step over/into/out, call stack, variables, globals, program input), "show ML", build/rebuild/clean, library build and install, and a toolchain doctor.
Opening a `.coy` or `.cyproj` file from Explorer (or `CoyoteStudio.exe file.coy`) opens it in the running Studio.

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

On Linux: the Studio unit tests, an end-to-end UI test under Electron (opening a file from the command line, diagnostics, run, Project Manager build/clean, the app window, settings and themes, completion for inheritance and imports, debugger, show ML), `electron-packager` producing the Windows app directory, and the installer script **compiled and exercised under Wine with a 32-bit test build**: fresh install, update, same-version repair, downgrade refusal, cli-only type, uninstall and PATH cleanup.
The installed `coyote.exe` (Wine, 32-bit test build) found `libs\standard.lib` and ran `use "Standard"` programs, including after an update.
**Not verified:** the real `windows-2022` CI job, an MSVC build, the packaged `CoyoteStudio.exe` actually running on Windows, and the installer's UI (only silent mode was exercised).

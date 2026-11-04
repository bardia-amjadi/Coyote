# Projects and the `.cyproj` file

A Coyote project is a folder with a **project file**:

* `<Name>.cyproj` — the project file used by Coyote Studio (like `MyApp.csproj`): `MyApp/MyApp.cyproj`.
* `packages.json` — the same format, still supported everywhere. If both exist, `packages.json` wins.

Both are JSON, validated by `cpx check` (schema version 1). `cpx`, `cpm`, `coyote lib pack` and `use "package"` all
find the project file in a project folder (`packages.json` first, otherwise the first `*.cyproj` by name).
`cpm init MyApp --cyproj` creates a `.cyproj` project (`cpm init MyApp` still creates `packages.json`).

## MyApp.cyproj

```json
{
  "schemaVersion": 1,
  "name": "MyApp",
  "version": "1.0.0",
  "type": "app",
  "entry": "src/main.coy",
  "description": "My first Coyote app",
  "output": { "directory": "bin", "name": "MyApp", "kind": "native", "mode": "auto", "keepUnused": false },
  "run": { "args": [] },
  "requires": [],
  "dependencies": {},
  "workflows": {}
}
```

| Key | Meaning |
|---|---|
| `name`, `version` | project identity (`name`: letters, digits, `-`, `_`, `.`) |
| `type` | `"app"` (runnable) or `"library"` (packed into a `.lib`, see [libraries.md](libraries.md)) |
| `entry` | the `.coy` file where the program starts (default `app.coy`) |
| `description` | free text |
| `output.directory` | where **Build** writes its result, relative to the project (default `bin`) |
| `output.name` | file name without extension (default: the project name) |
| `output.kind` | `"native"`: an executable (`MyApp.exe`); `"ml"`: a `.ml` file for the PVM |
| `output.mode` | native builds only: `"auto"` (real native code when the program fits, otherwise a bundled executable), `"bundle"` (always bundled), `"native-only"` (fail when the program does not fit) — see [llvm.md](llvm.md) |
| `output.keepUnused` | `true` keeps methods that are never called (see *Dead-code removal* in [compiler.md](compiler.md)) |
| `run.args` | arguments the program receives when started (`Environment.Arg`) |
| `requires`, `dependencies`, `workflows` | as in `packages.json` ([cpx.md](cpx.md), [cpm.md](cpm.md)) |

**How the settings are used.** When a project has an `output` section, its default workflows are:

* `build` — `coyote build <entry>` and, for `"native"`, `coyote llvm` writing `<directory>/<name>[.exe]`; for `"ml"` the
  `.ml` file goes straight to `<directory>/<name>.ml`;
* `start` — `coyote run <entry> -- <run.args>` (every permission, like Studio's Run).

You decide *what* a build produces in the project file — not with buttons: Studio's **Build** button, `cpx build` and
CI all read the same settings. Declaring your own `build` or `start` workflow replaces the default one.
Without an `output` section (a plain `packages.json`) the old defaults apply: `build` writes `.coyote/out/<entry>.ml`.

## Namespaces and the project folder

The folder that holds the project file is also the **namespace root**: `use "MyApp.Models"` finds the files under it whose first line is
`namespace MyApp.Models` (see [language](language.md)). `File > New Project` and `cpm init` start the entry file with `namespace <ProjectName>`
only when you write it yourself; existing programs without a namespace keep working.

## Coyote Studio: Project Manager

`View > Project Manager` (Ctrl+Shift+J) shows the open project:

* the **project tree** (source files, with the entry file marked), with *New file / New folder / Rename / Delete / Set as
  entry point* in the context menu;
* **Properties**: every key above as a form — saved into the `.cyproj` file when you press *Save* (comments and unknown
  keys of your own, such as `editor`, are preserved);
* **Build** (uses the output settings), **Rebuild**, **Clean** (removes `.coyote/out` and the output file),
  **Run**, **Debug**, and **Open output folder**.

`File > New Project` creates `<Name>/<Name>.cyproj` with `src/main.coy`. Opening a folder (or the `.cyproj` file itself)
opens the project.

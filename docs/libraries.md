# Libraries: `use`, `.lib` files and the Standard library

## Using a library

```
use "Standard"                 # by name: finds standard.lib in the library folders below
use "./My.lib"                 # by path, relative to the file that writes it

Shell.PrintLine("Hello world")
```

`use` is allowed only at the top level of a file. A library's exported classes and methods (`export class`, `export method`) become visible, exactly as with `import`. Name lookup is case-insensitive on the file name (`use "Standard"` finds `standard.lib`).

### Where `use "Name"` looks (first match wins)

1. `$COYOTE_LIBS` (a list of folders; `;` on Windows, `:` elsewhere)
2. `<project>/.coyote/libs/` (the folder of the file using the library and every folder above it)
3. the toolchain's own `libs` folder: `<install>\libs` (installed by the Windows setup next to `bin`) or `libs` next to the tools in a build folder
4. the current user's `~/coyote/libs` (`%USERPROFILE%\coyote\libs` on Windows) - "Build and Install Library for Me" in Studio puts libraries here

A missing library is an error that lists every folder it looked in.

## What a `.lib` is

A single text file that bundles a library's `.coy` source files (see `src/pkg/lib.hpp` for the format). It is a **source** bundle: `use` compiles the library's files together with your program, the same way `import` does. Consequences, stated plainly: there is no separate compilation, no binary compatibility, and a program contains the code of every class in a library it uses (unused ones included, because there is no dead-code removal yet). The native backend (`coyote-llvm`) cannot compile programs that use the Standard library yet, since it supports only `con.print`/`con.println` host functions.

## Writing a library (also in Coyote Studio: New Project, type `library`)

```
cpm init MyLib --library       # packages.json with "type": "library" and src/Mylib.coy
coyote lib pack MyLib          # -> mylib.lib (use -o to choose the path)
coyote lib info mylib.lib      # name, version, files
```

All `.coy` files under `src/` (or the project folder when there is no `src/`) are packed. Packing **compiles the library first**: errors are reported against your source files and nothing is written. Library files see each other's exports without `import`.

## Host functions (`hand:`) with return values

Libraries wrap PVM host functions. A declaration may now name a return type:

```
hand:math sqrt(double x): double
hand:con readLine(): string
```

Return types: `void` (default), `int`, `double`, `boolean`, `char`, `string`. The PVM runs a host function only if it is registered, its signature matches, and its permission is granted. Permissions: `console`, `time` and `random` are granted by default; functions with no permission (`math`, `text`) are pure.

| Module | Functions | Permission |
|---|---|---|
| `con` | `print`, `println`, `readLine(): string`, `hasInput(): boolean` | `console` |
| `math` | `sqrt pow sin cos tan asin acos atan atan2 exp log log10 floor ceil round` | none |
| `text` | `length substring charAt indexOf upper lower trim replace` (character based; `upper`/`lower` ASCII only) | none |
| `time` | `uptimeMs unixSeconds sleep format` | `time` |
| `random` | `seed nextInt nextDouble` | `random` |
| `file` | `readAll writeAll append exists delete copy move size modified fullPath` | `file` |
| `dir` | `exists create delete list` | `file` |
| `env` | `get has set` | `system` |
| `sys` | `argCount arg exit cwd os homeDir` | `system` |
| `http` | `request body headers header error` (Windows: http and https; other systems: http only) | `net` |

Permissions are checked when a host function is **called**, so a program may declare functions it never reaches. `coyote run` and Coyote Studio grant every permission; the bare `pvm` grants `console`, `time` and `random` unless you pass `--allow <permission>` or `--allow-all`. A denied call is a runtime error naming the permission.

Under the debugger, `readLine` works too: the PVM sends an `inputRequested` event and waits for a `stdin` command ([debugging](debugging.md)); Studio shows this as a prompt in the Terminal.

## The Standard libraries (`stdlib/`, installed as `.lib` files)

`use "Standard"` is an umbrella library with no code of its own: it `requires` all the others. Use a single one (`use "Standard.IO"`) to keep programs small. A library can list `requires` in its `packages.json` (`"requires": ["Other"]`); required libraries are loaded automatically and are visible to anyone who uses the library.

| Library | Classes |
|---|---|
| `Standard.IO` | `Shell` (`Print PrintLine ReadLine ReadInt ReadDouble HasInput Prompt`), `File`, `Directory`, `Path` |
| `Standard.Text` | `Text` (`Length IsEmpty IsBlank Substring CharAt IndexOf Contains StartsWith EndsWith Upper Lower Trim Replace Repeat PadLeft PadRight Join` ...) |
| `Standard.Math` | `Math`, `Random` |
| `Standard.Time` | `Time` |
| `Standard.System` | `Environment` (environment variables, arguments, `Exit`, current directory, OS) |
| `Standard.Http` | `Http` (`Get Post PostJson Send`), `HttpResponse` |

Hover or complete any member in Coyote Studio to see its documentation (the `#` comment lines above each member in `stdlib/`).

There is no method overloading yet, so names such as `AbsInt` / `MinInt` exist next to `Abs` / `Min`.

## Not implemented


Library versions are recorded but not checked or resolved (`use` takes the first library it finds); `cpm` does not install libraries; there is no `.lib` signing; no way to hide a library's source.

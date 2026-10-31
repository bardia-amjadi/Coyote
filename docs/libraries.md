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

| Module | Functions |
|---|---|
| `con` (console) | `print`, `println`, `readLine(): string`, `hasInput(): boolean` |
| `math` | `sqrt pow sin cos tan asin acos atan atan2 exp log log10 floor ceil round` (all `double`) |
| `text` | `length substring charAt indexOf upper lower trim` (character-based; `upper`/`lower` are ASCII only) |
| `time` | `uptimeMs(): int`, `unixSeconds(): int`, `sleep(int ms)` |
| `random` | `seed(int)`, `nextInt(int low, int high): int`, `nextDouble(): double` |

While a program runs under the debugger, `con.readLine` reports that no input is available (stdin carries debugger commands).

## The Standard library (`standard.lib`, from `stdlib/`)

| Class | Members |
|---|---|
| `Shell` | `Print(text)` (no line break), `PrintLine(text)`, `ReadLine()`, `HasInput()` |
| `Math` | `PI`, `E`, `Sqrt Pow Sin Cos Tan Asin Acos Atan Atan2 Exp Log Log10 Floor Ceil Round`, `Abs`, `AbsInt`, `Min`, `Max`, `MinInt`, `MaxInt` |
| `Text` | `Length IsEmpty Substring CharAt IndexOf Contains StartsWith EndsWith Upper Lower Trim Repeat` |
| `Time` | `UptimeMs UnixSeconds Sleep` |
| `Random` | `Seed NextInt NextDouble` |

There is no method overloading yet, so names such as `AbsInt` / `MinInt` exist next to `Abs` / `Min`.

## Not implemented

Library versions are recorded but not checked or resolved (`use` takes the first library it finds); `cpm` does not install libraries; there is no `.lib` signing; no way to hide a library's source.

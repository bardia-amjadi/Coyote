# Libraries: `use`, `.lib` files and the Standard library

## Using a library

```
use "Standard"                 # by name: finds standard.lib in the library folders below
use "./My.lib"                 # by path, relative to the file that writes it

Shell.PrintLine("Hello world")
```

`use` is allowed only at the top level of a file. A library's exported classes and methods (`export class`, `export method`) become visible, exactly as the exports of a namespace do (see [language](language.md), *Namespaces, `use` and `export`*). `use "Name"` also finds namespaces of your own project and installed packages; if a name matches more than one kind it is reported as ambiguous. Name lookup is case-insensitive on the file name (`use "Standard"` finds `standard.lib`).

### Where `use "Name"` looks (first match wins)

1. `$COYOTE_LIBS` (a list of folders; `;` on Windows, `:` elsewhere)
2. `<project>/.coyote/libs/` (the folder of the file using the library and every folder above it)
3. the toolchain's own `libs` folder: `<install>\libs` (installed by the Windows setup next to `bin`) or `libs` next to the tools in a build folder
4. the current user's `~/coyote/libs` (`%USERPROFILE%\coyote\libs` on Windows) - "Build and Install Library for Me" in Studio puts libraries here

A missing library is an error that lists every folder it looked in.

## What a `.lib` is

A single text file that bundles a library's `.coy` source files (see `src/pkg/lib.hpp` for the format). It is a **source** bundle: `use` compiles the library's files together with your program, the same way the files of a namespace are. Consequences, stated plainly: there is no separate compilation, no binary compatibility, and but the compiler removes methods your program never calls (dead-code removal, see [compiler.md](compiler.md)), so using a big library costs only what you use. `coyote-llvm` turns programs that use libraries into bundled executables (see [llvm.md](llvm.md)).

## Writing a library (also in Coyote Studio: New Project, type `library`)

```
cpm init MyLib --library       # packages.json with "type": "library" and src/Mylib.coy
coyote lib pack MyLib          # -> mylib.lib (use -o to choose the path)
coyote lib info mylib.lib      # name, version, files
```

All `.coy` files under `src/` (or the project folder when there is no `src/`) are packed. Packing **compiles the library first**: errors are reported against your source files and nothing is written. Library files see each other's exports without `use`.

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
| `Standard.Text` | `Text` (`Length IsEmpty IsBlank Substring CharAt IndexOf Contains StartsWith EndsWith Upper Lower Trim Replace Repeat PadLeft PadRight Join` ...; also usable as extension methods: `s.Upper()`), `StringBuilder` (`Append AppendLine ToString Length Clear`) |
| `Standard.Math` | `Math`, `Random`, `Numbers` (`Gcd Lcm IsPrime Factorial Fibonacci ClampInt Sign ToRadians ToDegrees`) |
| `Standard.Time` | `Time` |
| `Standard.System` | `Environment` (environment variables, arguments, `Exit`, current directory, OS) |
| `Standard.Http` | `Http` (`Get Post PostJson Send`), `HttpResponse` |
| `Standard.Collections` | generic `List<T>`, `Stack<T>`, `Queue<T>`, `Set<T>`, `Pair<A, B>`; `Arrays` (`SumInt SortInt MaxInt AverageInt` ...); generic `Reversed<T>`, `IndexOfValue<T>`, `ContainsValue<T>` |
| `Standard.Testing` | `Assert` (`True False EqualInt EqualString EqualDouble Fail Finish`) |

Hover or complete any member in Coyote Studio to see its documentation (the `#` comment lines above each member in `stdlib/`).

Method overloading exists since 0.6.1, but the Standard libraries have not been cleaned up yet (planned for 0.6.4), so names such as `AbsInt` / `MinInt` exist next to `Abs` / `Min`.

## Not implemented


Library versions are recorded but not checked or resolved (`use` takes the first library it finds); `cpm` does not install libraries; there is no `.lib` signing; no way to hide a library's source.

### Collections example

```
use "Standard"

List<int> xs = new List<int>()          # generic: the element type goes in angle brackets
xs.Add(3)
xs.Add(1)
Shell.PrintLine("count " + xs.Count)     # Count is a property
for (var x in xs.ToArray()) { Shell.PrintLine("" + x) }

Stack<string> st = new Stack<string>()
st.Push("a")
Shell.PrintLine(st.Pop())

Assert.EqualInt(2, xs.Count, "two elements")   # Standard.Testing
Assert.Finish()                                  # prints "1 passed, 0 failed"; exit code 1 on failures
```

Collections compare elements with `==`, so lists of `int`, `string`, `double`, `char` and `boolean` compare by value and lists of objects by identity.
Generic methods of the library need their type argument (`Reversed<int>(xs)`); see *Generics* in [language.md](language.md).

## Source bundles, not compiled libraries (status)

A `.lib` is a **bundle of source files**: `use "Name"` compiles those sources together with your program (and dead-code
removal then drops what you do not call). There is **no separately compiled library format** yet (precompiled ML with
headers, linking at load time). That needs a design for exported signatures, generic templates (which must stay as
source) and ML-level linking; it is planned, not started, so nothing here claims it.

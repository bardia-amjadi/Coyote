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
| `Standard.Text` | String extension methods (0.6.5; the static class `Text` was removed): `Length IsEmpty IsBlank Substring CharAt IndexOf Contains StartsWith EndsWith Upper Lower Trim Replace Repeat PadLeft PadRight Reverse Split ToInt ToDouble` on a string (`s.Upper()`), `Join` on an `array<string>`; `StringBuilder` (`Append AppendLine ToString Length Clear`) |
| `Standard.Math` | `Math`, `Random`, `Numbers` (`Gcd Lcm IsPrime Factorial Fibonacci ClampInt Sign ToRadians ToDegrees`, formatting since 0.10: `ToBase Hex Binary Thousands Fixed`) |
| `Standard.Time` | `Time` (clocks, `Format`; since 0.10 calendar parts in UTC: `IsLeapYear DaysInMonth FromParts Year Month Day Hour Minute Second DayOfWeek DayOfYear FloorDiv`) |
| `Standard.System` | `Environment` (environment variables, arguments, `Exit`, current directory, OS) |
| `Standard.Http` | `Http` (`Get Post PostJson Send`), `HttpResponse` |
| `Standard.Collections` | generic `List<T>`, `Stack<T>`, `Queue<T>`, `Set<T>`, `Pair<A, B>`; `Arrays` (`SumInt SortInt MaxInt AverageInt` ...); generic `Reversed<T>`, `IndexOfValue<T>`, `ContainsValue<T>`; since 0.10 `Sorted<T>(xs, comparer)` (stable merge sort) and `List<T>.Sort(comparer)` |
| `Standard.Windows` | (0.6.3, Windows only; needs the `windows` permission) `MessageBox` (`Show ShowInfo ShowWarning ShowError Ask Confirm`, returns `DialogResult`), `DialogButtons`, `DialogIcon`, `FileDialogs` (`OpenFile SaveFile Folder`), `Clipboard` (`GetText SetText HasText Clear`), `Registry` (`Read ReadInt ReadOr Exists KeyExists Write WriteInt DeleteValue DeleteKey SubKeys Values`), `Processes` (`Open List Find`; starting, waiting and killing programs is in Standard.Process), `Windows` (`List Find Title SetTitle Hide Show Minimize Maximize Restore Close Focus Bounds Move`), `Notifications` (`Show`) |
| `Standard.Encoding` | (0.6.4) `Encoding` (`CodePoints FromCodePoints`), `Utf8` `Ascii` `Latin1` `Utf16` (`GetBytes GetString`), `Base64` (`Encode Decode EncodeUrlSafe DecodeUrlSafe EncodeText DecodeText`), `Hex`, `Url` (`Encode Decode`), `Html` (`Encode Decode`). Pure Coyote, no permission. A byte array is an `array<int>` of 0..255 |
| `Standard.Process` | (0.6.4, `process` permission) `Process` (`Run RunFull Shell Start StartIn Which Exists CurrentId FromId`; on a started process `IsRunning Wait WaitFor Kill Id`), `ProcessResult` (`ExitCode Output Error TimedOut Succeeded`) |
| `Standard.Runtime` | (0.6.4, `system` permission) `Runtime` (`Version Architecture ProcessorCount MachineName UserName ProcessId MemoryBytes GcCollections GcFreedObjects HeapObjects HeapElements Describe`) |
| `Standard.Resources` | (0.6.4, `file` permission) `Resources` (`Root SetRoot PathOf Exists Text Lines Names NamesIn`), `ResourceBundle` (`Load Has Get GetOr Format Count`) |
| `Standard.Configuration` | (0.6.4, `file`/`system`) `Configuration` (INI-style: `Load LoadOrEmpty Parse WithEnvironment Has Get GetOr GetInt GetDouble GetBool Set Remove Keys Count ToText Save`) |
| `Standard.Coyote.InteropServices` | (0.6.4, `interop` + `process`) `CoyoteModule` (`Load Run Functions Has Call CallInt CallBool CallDouble Output Unload`), `ProgramResult`, `CoyoteCompiler` (`SetPath Find Build`), `CoyoteProgram` (`FromSource`) |
| `Standard.Windows.InteropServices` | (0.6.4, Windows + 64-bit only, `interop`) `NativeLibrary` (`Load Exists HasFunction Function`), `NativeCall` (`AddInt AddBool AddAnsi AddWide AddOutBuffer Invoke InvokeInt InvokeUInt InvokeLong InvokeBool InvokeAnsi InvokeWide BufferAnsi BufferWide`) |
| `Community.Text` | (0.6.4, see *Community* below) `Regex`, `RegexOptions`, `Match`, `Group` |
| `Community.Timer` | (0.6.4) `Stopwatch`, `Timer`, `Timers` |
| `Standard.Testing` | `Assert` (`True False EqualInt EqualString EqualDouble Fail Finish`) |

Hover or complete any member in Coyote Studio to see its documentation (the `#` comment lines above each member in `stdlib/`).

Method overloading exists since 0.6.1, but the Standard libraries have not been cleaned up yet (planned for 0.6.4), so names such as `AbsInt` / `MinInt` exist next to `Abs` / `Min`.

## Standard.Windows (0.6.3)

The `win` host module backs a library of ordinary Coyote classes, so a dialog result is a real type rather than an int:

```
use "Standard"
var r = MessageBox.Show("Save changes?", "Coyote", DialogButtons.YesNoCancel, DialogIcon.Question)
if (r == DialogResult.Yes) { Shell.PrintLine("saving") }       # DialogResult values are shared objects, so == compares identity correctly
if (MessageBox.Ask("Continue?", "Coyote")) { }                 # Yes/No as a boolean
string path = FileDialogs.OpenFile("Open", "Text|*.txt|All|*.*")
Clipboard.SetText("hello")
Registry.Write("HKCU\\Software\\Demo", "Name", "Coyote")
```

**Modern look (0.6.5.1).** Every Windows `.exe` of the toolchain (and so every bundled executable, which is a copy of `pvm.exe`) embeds an application manifest
(`src/win/coyote.manifest`): Common Controls 6 (themed controls) and per-monitor-v2 DPI awareness (sharp text on high-DPI screens). `MessageBox` is shown as a Windows
**TaskDialog** (the current Windows dialog style) instead of the legacy `MessageBoxW` box: a text with a line break (`"Save changes?\nYour document has unsaved edits."`)
shows the first line as the large headline and the rest as the content; a single line is plain content. The buttons, icons and return values are the same as before.
`AbortRetryIgnore` and `CancelTryContinue` have no localized common buttons in a TaskDialog, so those two use the classic box (still themed). If comctl32 v6 is not
active the classic box is used for everything. `FileDialogs` still use the classic Windows file dialogs (themed, but the old layout); the modern `IFileDialog` is not done yet.
Natively compiled (clang) executables carry no manifest yet, but they cannot call `win.*` functions anyway (they are outside the native subset and become bundled executables).

* Declared with `hand:win name(...)` (primitive parameter types only; the library wraps them). The functions need the **`windows`** permission
  (`coyote run` and Studio grant everything; `pvm --allow windows`). Without it a call is a runtime error before anything happens.
* On a system that is not Windows every function fails with `host function 'win.<name>' failed: the win.* functions work only on Windows` (a catchable error).
* Not native: a program using it runs as a bundled executable (PVM inside) under `coyote-llvm`.
* Registry keys are written `HKCU\\Software\\X` (also `HKLM`, `HKCR`, `HKU`, `HKCC`); strings are UTF-8 on the Coyote side.

## 0.7: long values and bytes

`Time.UptimeMs()`, `Time.UnixSeconds()`, `File.Size()`, `File.ModifiedUnix()` return `long` (no more 24-day / 2038 / 2 GiB limits); `Time.Format(long, ...)`/`FormatUtc`. New:
`File.ReadAllBytes(path): array<byte>`, `File.WriteAllBytes(path, data)`, `File.AppendBytes(path, data)` (permission `file`), `Encoding.ToBytes(text)`, `Encoding.FromBytes(data)`, `Encoding.IntsToBytes`, `Encoding.BytesToInts`.
Host functions (`hand:`) may now use `long`, `byte` and `array<byte>`.

## Removed duplicates (0.6.5)

Each function now has one home. Run `python3 tools/migrate-0.6.5.py yourfile.coy` to rewrite most uses.

| Removed | Use instead |
| --- | --- |
| `Text.Trim(s)` and every other `Text.X(s, ...)` (class `Text`) | `s.Trim()` ... (string extension methods); `Text.Join(parts, sep)` is `parts.Join(sep)` |
| `Processes.Start/Run/IsRunning/Kill/CurrentId` (Standard.Windows) | `Process.Start/Run`, `Process.FromId(id).IsRunning()/.Kill()`, `Process.CurrentId()` (Standard.Process; arguments are a list, not one string) |
| `Runtime.OS()`, `Runtime.UptimeMs()` | `Environment.OS()`, `Time.UptimeMs()` |
| `ResourceFolder.Names(folder)` | `Resources.NamesIn(folder)` |
| host functions `win.processStart/processRun/processIsRunning/processKill/processId` | `proc.*` |

`Process.Kill` (host `proc.kill`) refuses to kill the program's own process.

## The 0.6.4 Standard libraries

* **Encoding**: `Base64.EncodeText("Hello")`, `Hex.Encode(Utf8.GetBytes("é"))`, `Url.Encode("a b&c")`, `Html.Encode("<b>")`. Text is counted in characters (code points) like Standard.Text; `GetString` turns a broken byte sequence into U+FFFD, `Base64.Decode`/`Hex.Decode`/`Url.Decode` stop with an error on bad input.
* **Process**: arguments are passed one by one, never through a shell (use `Process.Shell("a | b")` when you want one). `Process.Run("git", ["status"])` waits and captures standard output and error (up to 32 MB each); `RunFull(program, args, folder, input, timeoutMs)` adds a working folder, standard input text and a time limit (the program is killed, `TimedOut` is true, `ExitCode` is -1). `Process.Start` returns a handle without waiting; its streams go nowhere. Input is written before output is read on Windows, so keep it small (a few tens of KB). A program that cannot be started is a catchable error. Needs the `process` permission.
* **Runtime**: information from the VM (`Runtime.Version()` is the Coyote version, `GcCollections()` how often the collector ran). `MemoryBytes()` is `-1` on Windows. A native `coyote-llvm` program that needs any of these is a bundled executable, so the numbers describe the PVM inside.
* **Resources**: `Resources.Text("licence.txt")` reads `resources/licence.txt` (change the folder with `Resources.SetRoot`). The files are read from disk at run time, they are not embedded in the executable. `ResourceBundle.Load("messages", "fa")` reads `messages.fa.properties` first, then `messages.properties` (`key = value` lines, `#` comments); `Format("hello", ["Bardia"])` replaces `{0}`.
* **Configuration**: INI files (`# comment`, `name = value`, `[section]`; values may be quoted). A setting in a section is named `section.name`. `WithEnvironment("MYAPP_")` lets variables such as `MYAPP_SERVER_PORT` override `server.port`. `Save` writes settings without a dot first, then one section per prefix (comments are not kept).
* **Coyote interop**: `CoyoteModule.Load("plugin.ml")` loads and verifies an ML file in the running VM. `Run()` runs its top-level code and captures what it prints; `Call("Add", ["20", "22"])` calls a function by name with arguments written as text and returns the result as text (`CallInt`, `CallBool`, `CallDouble` convert). Only `int`, `boolean`, `double` and `string` parameters/results can cross; a runtime error inside the function becomes a catchable error. Every call starts with fresh global variables. The compiler removes methods nothing calls, so build plugins with `coyote build --keep-unused` (`CoyoteCompiler.Build` / `CoyoteProgram.FromSource` do this by running the installed `coyote`, found through the `COYOTE_COMPILER` variable, `CoyoteCompiler.SetPath` or `PATH`). The loaded program can call everything the host program is allowed to; permissions are not narrowed.
* **Windows interop** (`NativeLibrary`, `NativeCall`): calls a DLL export with up to 8 arguments (integers/pointers, booleans, ANSI or UTF-16 text, output buffers) and reads the result as 32-bit signed/unsigned, 64-bit, boolean or text. Windows cannot check the signature: a wrong name, count or type can crash the program, so call only functions you know. 64-bit builds only. Needs the `interop` permission.

## Community (0.6.4)

The `Community` family (`use "Community"` for all of it, or `use "Community.Text"`, `use "Community.Timer"`) is written entirely in Coyote on top of the Standard libraries, with **no `hand:` declarations**: it needs no host support and no permissions, and it is compiled into every program that uses it (a regular-expression engine is therefore part of your application, not of the runtime). Standard libraries are the host-facing layer; Community libraries are everything that can be built from them.

* **`Community.Text`**: `Regex`. `new Regex("(\\d+)-(\\d+)")`, then `IsMatch`, `Match`, `MatchFrom`, `Matches`, `Replace` (`$1`, `${name}`, `$0`, `$$`), `ReplaceFirst`, `Split`; `new Regex(pattern, RegexOptions.IgnoreCase + RegexOptions.Multiline)`, `Regex.Escape`, `Regex.IsMatchText`, `Regex.ReplaceText`. A `Match` has `Success Value Index Length Groups`, `Text(n)`, `GetGroup(n)`, `GetNamed("name")`. Supported: literals, `.`, `[sets]` and ranges, `\d \w \s` (and capitals), `\b \B \A \z \Z ^ $`, groups, `(?:)`, named groups `(?<n>)`, `|`, `* + ? {n,m}` with lazy `?` versions, back references `\1` and `\k<n>`, look-ahead `(?= )` `(?! )`, inline flags `(?i) (?m) (?s) (?i: )`, escapes `\n \t \xHH \uHHHH`. Not supported: look-behind, atomic groups, possessive quantifiers, `\p{..}`. It is a backtracking engine: a pattern such as `(a*)*b` on a long text of `a` takes exponential time, so every search stops with an error after `StepLimit` steps (default 20 million). Case folding covers ASCII, Latin-1, Greek and Cyrillic. Positions are in characters, as in Standard.Text.
* **`Community.Timer`**: `Stopwatch` (`StartNew Start Stop Reset Restart ElapsedMs`), and cooperative timers: `Timer.After(ms, fn)`, `Timer.Every(ms, fn)`, `Timer.RunAll()`, `Timer.StopAll()`, or your own `Timers` loop (`Add RunOnce Run RunFor RequestStop Count`). Programs have one thread, so timers fire only while a timer loop runs. `ElapsedMs()` and `DueIn()` return `long` (0.7).

## Not implemented


Library versions are recorded but not checked or resolved (`use` takes the first library it finds); `cpm` does not install libraries; there is no `.lib` signing; no way to hide a library's source.

### Collections example

```
use "Standard"

List<int> xs = new List<int>()          # generic: the element type goes in angle brackets
xs.Add(3)
xs.Add(1)
Shell.PrintLine("count " + xs.Count)     # Count is a property
in (var x of xs.ToArray()) { Shell.PrintLine("" + x) }

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

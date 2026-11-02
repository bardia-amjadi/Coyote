# Debugging

`pvm --debug program.ml` runs a program under a line-level debugger controlled over stdin/stdout.
Coyote Studio drives it for you (F6, breakpoints in the gutter, step buttons); this page describes the protocol so other tools can too.

Build with `coyote build file.coy -o file.ml`. The compiler records a `.line` at every statement and a `.var` name for every parameter and local, so the debugger shows source lines and variable names.

## Behaviour

* The program starts **paused at its first source line** (`reason: "entry"`).
* A line event fires when the file, line, function or call depth changes.
* End of stdin means `quit`.
* Program output is reported as `output` events instead of being written raw.
* Hidden compiler temporaries (names starting with `$`) are not shown.

## Commands (one per line on stdin)

| Command | Effect |
|---|---|
| `break <file> <line>` / `unbreak <file> <line>` / `clearbreaks` | breakpoints; file matching is path-normalised with suffix matching |
| `continue` | run to the next breakpoint, runtime error or exit |
| `step` | step into the next line |
| `next` | step over calls |
| `out` | run until the current function returns |
| `stack` | call stack |
| `vars <frame>` | variables of a frame (0 = innermost) |
| `globals` | global variables |
| `quit` | stop |
| `stdin "<json string>"` | answers a pending `inputRequested`: one line of program input (the line break is added) |
| `stdineof` | answers a pending `inputRequested` with end of input |

## Events (one JSON object per line on stdout)

`stopped {reason: entry|breakpoint|step, file, line, function}`, `output {text}`, `stack`, `vars`, `globals`, `breakpoint`, `breakpoints-cleared`, `runtimeError {message}`, `exited {code}`, `error {message}`, `inputRequested` (the program called `readLine` and waits for a `stdin` or `stdineof` command; the program's prompt is always sent as an `output` event first).

Values are described with depth and size limits (large arrays and deep objects are truncated).

## Limits

* Statement granularity: breakpoints bind to statement starts, not to sub-expressions.
* No expression evaluation, watch expressions or conditional breakpoints.
* No attach-to-running-process; no debugging of native (`coyote-llvm`) executables.

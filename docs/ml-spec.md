# ML (Middle Language) specification — version 6

ML is the textual, versioned interface between the Coyote compiler and execution backends. The
compiler (`coyote`) emits it; the PVM (`pvm`) loads, verifies and executes it. The two programs
share **no code**, only this document.

Versions: the compiler emits **version 5**, **6** when the program uses function values (lambdas, `fn` types), **7** for class hierarchies and **8** when it uses `try`/`throw` (the highest needed). The PVM accepts versions **1 to 8**. Each version is a
strict superset of the previous one; a file that uses a construct newer than its declared version
is rejected with a message naming the construct. Any future change that breaks existing ML bumps the version.

## File structure

ML is line-oriented UTF-8 text (`\n` or `\r\n`). Blank lines are ignored, and a line whose first
non-blank character is `;` is a comment. Words are separated by spaces or tabs; `(`, `)` and `,`
are always separate words.

Order of items:

1. `ml <version>` — the first non-comment line.
2. `source "<path>"` — the original `.coy` path, used in runtime error messages. Exactly one.
3. Zero or more `host` declarations.
4. One or more `func ... end` blocks.
5. `entry <function>` — exactly one; names the function to run.

Function and label names are any run of characters other than whitespace, quotes, parentheses and
commas. The compiler names the top-level code function `$main`, which cannot collide with a
Coyote method name.

## Types

| Type      | Meaning                                       | Since |
|-----------|-----------------------------------------------|-------|
| `int`     | 32-bit signed integer                         | 1 |
| `string`  | immutable byte string (UTF-8 by convention)   | 1 |
| `boolean` | `true` or `false`                             | 2 |
| `array<T>`| dynamic array of `T` (any non-void type, including arrays); a reference to a garbage-collected heap object | 3 |
| `double`  | IEEE-754 binary64                             | 4 |
| `char`    | one Unicode code point (not a surrogate)      | 4 |
| `Guid`    | UUID, stored as canonical lowercase text `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx` | 4 |
| `map<K,V>`| insertion-ordered map; `K` is `int`, `string`, `char`, `boolean` or `Guid`; `V` is any non-void type; a heap object | 4 |
| `T?`      | nullable `T` (one level only; `T` is not void, nullable or `obj`); a value that may be null | 4 |
| `obj`     | holds a value of any non-void type, chosen at run time (replaced on each store); may be null | 4 |
| `fn(T,U)->R` | function value (a closure): a function plus captured values; a heap object, may be a null reference. Written without spaces; `R` may be `void` or any type, parameters are not void. Function types are never nullable (`fn(int)->int?` returns `int?`) and cannot be boxed into `obj` or used in host signatures | 6 |
| `<Name>`  | instance of a declared class (see "Classes and globals"); a heap object, may be a null reference | 5 |
| `void`    | no value; return type only                    | 1 |

In text, a map type is written without spaces (`map<string,int>`). `dyn` exists only in the
language: it is `obj` at the ML level, with the `dyn.*` instructions below.

Array types nest at most 32 deep. Declared array locals and fresh `newarray` elements start as the
element type's default (`0`, `""`, `false`, or a **null reference** for nested arrays). A null
reference is a legal value; using it in an array instruction is the runtime error
`null array reference`.

## String literals

`"..."` with escapes `\\`, `\"`, `\n`, `\t`, `\r` and `\xHH` (two hex digits, one byte). Strings
cannot span lines.

## Declarations

```
host <module>.<name> (<type>, ...) -> <return type>
func <name> (<type>, ...) -> <return type>
  local <type>              ; version 2: zero or more, before the first instruction
  ...instructions, labels...
end
entry <name>
```

* `host` declares a host intrinsic (see "Host calls and permissions"). Declaring grants nothing.
* **Locals.** A function's variable slots are numbered from 0: its parameters first, in order,
  then each `local` in declaration order. Slots hold a value of their declared type for the whole
  call. Declared locals start as `0`, `""` or `false`; parameters start as the passed arguments.
* The entry function must have signature `() -> void`.

## Classes and globals (version 5)

```
class <name>
  field <name> <type>
  ...
end
global <name> <type>
```

* Class and global declarations come before the first `func`. Class and field names may contain
  letters, digits, `_`, `.` and `$` (the compiler uses `m1.Name` for classes of imported files and
  `Class.field` for static fields). Names are unique per kind. A class type is written as its name;
  every named class must be declared (a class may refer to itself or to later classes). Host
  signatures cannot use class types.
* An instance is a heap object whose fields start as the type's default: `0`, `""`, `false`, `0.0`,
  and a **null reference** for class, array, map, `T?` and `obj` fields. A class-typed value can be
  null; using it in `getfield`/`setfield` is the runtime error `null reference: ...`.
* `newobj C` pushes a new instance; `getfield C.f` (object -> value) and `setfield C.f`
  (object, value ->) read/write a field (the verifier checks the object's class and the value type);
  `getglobal g` / `setglobal g` read/write a global, which starts at its type's default.
* `eq.ref` / `ne.ref` compare class instances by identity.
* The collector marks fields of reachable instances and all globals; `newobj` is an allocating instruction.

## Function values (version 6)

* `mkclosure <function> <n>` pops `n` values (bottom to top), which must have the types of the
  function's **first** `n` parameters, and pushes a function value of type
  `fn(<remaining parameter types>)-><return type>`. With `n = 0` it is a plain reference to the function.
* `callfn <argc>` expects a function value below `argc` arguments (bottom to top: function, args). The
  arguments must match the function type; it calls the function with the captured values followed by the
  arguments and pushes the result unless the return type is `void`. A null function reference is the
  runtime error `null reference: cannot call a null function value`. Calls count against the call-depth limit.
* A function value is a copy of the captured values taken when it is made (capture by value). Captured
  values that are references (arrays, maps, objects, other function values) are shared, as everywhere in ML.
* `eq.ref` / `ne.ref` compare function values by identity. Function-typed locals, fields, globals, array
  and map elements start as a null reference. The collector marks captured values.

## Instructions

The machine is a typed operand stack; every function call has its own empty stack and its own
slots. Operands are listed bottom to top.

| Instruction             | Stack effect                         | Since | Notes |
|-------------------------|--------------------------------------|-------|-------|
| `push.str "<text>"`     | `-> string`                          | 1 | |
| `push.int <n>`          | `-> int`                             | 1 | `n` fits in 32 bits |
| `push.bool true\|false` | `-> boolean`                         | 2 | |
| `load <slot>`           | `-> value`                           | 2 | pushes a copy of the slot |
| `store <slot>`          | `value ->`                           | 2 | type must equal the slot's type |
| `add.int` `sub.int` `mul.int` | `int int -> int`               | 2 | overflow is a runtime error |
| `div.int` `rem.int`     | `int int -> int`                     | 2 | truncates toward zero; remainder has the dividend's sign; zero divisor and `INT_MIN / -1` are runtime errors |
| `neg.int`               | `int -> int`                         | 2 | `-INT_MIN` is a runtime error |
| `concat`                | `string string -> string`            | 2 | |
| `str.int` `str.bool`    | `int -> string`, `boolean -> string` | 2 | decimal digits with `-`; `true`/`false` |
| `eq.int` `ne.int` `lt.int` `le.int` `gt.int` `ge.int` | `int int -> boolean` | 2 | |
| `eq.str` `ne.str`       | `string string -> boolean`           | 2 | byte-wise equality |
| `eq.bool` `ne.bool`     | `boolean boolean -> boolean`         | 2 | |
| `not`                   | `boolean -> boolean`                 | 2 | |
| `jump <label>`          | no stack change                      | 2 | unconditional |
| `jump.false <label>`    | `boolean ->`                         | 2 | jumps when the value is `false`, otherwise continues |
| `call <func> <n>`       | `arg1 ... argn -> [result]`          | 2 | `n` equals the callee's parameter count; pushes a result only if the callee returns non-void |
| `hostcall <host> <n>`   | `arg1 ... argn -> [result]`          | 1 | as `call`, for a declared host function |
| `newarray <T>`          | `int -> array<T>`                    | 3 | negative size is a runtime error; elements are default values |
| `arraylit <T> <n>`      | `T1 ... Tn -> array<T>`              | 3 | builds an array from the top `n` values |
| `aload`                 | `array<T> int -> T`                  | 3 | bounds and null checked |
| `astore`                | `array<T> int T ->`                  | 3 | bounds and null checked |
| `alen`                  | `array<T> -> int`                    | 3 | |
| `apush`                 | `array<T> T ->`                      | 3 | appends |
| `acopy`                 | `array<T> -> array<T>`               | 3 | shallow copy (nested arrays are shared) |
| `eq.ref` `ne.ref`       | `array<T> array<T> -> boolean`       | 3 | reference identity, not content (version 4: also maps; version 5: also class instances) |
| `push.dbl <x>`          | `-> double`                          | 4 | decimal, or `inf`, `-inf`, `nan` |
| `push.chr <n>`          | `-> char`                            | 4 | code point `n`, not a surrogate |
| `add.dbl` `sub.dbl` `mul.dbl` `div.dbl` `rem.dbl` | `double double -> double` | 4 | IEEE semantics (no errors; `x/0` is infinity or NaN); `rem` is `fmod` |
| `neg.dbl`               | `double -> double`                   | 4 | |
| `eq.dbl` `ne.dbl` `lt.dbl` `le.dbl` `gt.dbl` `ge.dbl` | `double double -> boolean` | 4 | NaN compares false (except `ne`) |
| `eq.chr` `ne.chr` `lt.chr` `le.chr` `gt.chr` `ge.chr` | `char char -> boolean` | 4 | by code point |
| `eq.guid` `ne.guid`     | `Guid Guid -> boolean`               | 4 | |
| `i2d`                   | `int -> double`                      | 4 | |
| `d2i`                   | `double -> int`                      | 4 | truncates toward zero; NaN or out of `int` range is a runtime error |
| `c2i` / `i2c`           | `char -> int` / `int -> char`        | 4 | `i2c` rejects non-code-points |
| `str.dbl` `str.chr` `str.guid` | `T -> string`                 | 4 | doubles print as the shortest text that round-trips, with `.0` added when integral; `NaN`, `Infinity`, `-Infinity` |
| `parse.int` `parse.dbl` `parse.guid` | `string -> T`           | 4 | strict: whole string, no spaces; failure is a runtime error |
| `push.null <T?\|obj>`   | `-> T?` / `-> obj`                   | 4 | |
| `wrap`                  | `T -> T?`                            | 4 | |
| `unwrap`                | `T? -> T`                            | 4 | null is a runtime error |
| `isnull`                | `T? -> boolean`, `obj -> boolean`    | 4 | |
| `dup`                   | `T -> T T`                           | 4 | |
| `newmap <map type>`     | `-> map<K,V>`                        | 4 | |
| `mapget`                | `map<K,V> K -> V`                    | 4 | missing key is a runtime error |
| `mapset`                | `map<K,V> K V ->`                    | 4 | inserts at the end, or replaces in place |
| `maphas` / `mapdel`     | `map<K,V> K -> boolean`              | 4 | `mapdel` returns whether the key existed |
| `maplen`                | `map<K,V> -> int`                    | 4 | |
| `mapkeys` / `mapvalues` | `map<K,V> -> array<K>` / `array<V>`  | 4 | snapshot in insertion order |
| `maptry`                | `map<K,V> K -> V?`                   | 4 | null when missing (`V` itself when `V` is nullable or `obj`) |
| `box`                   | `T -> obj`                           | 4 | boxing null (or a null nullable) gives null `obj`; boxing an `obj` is a no-op |
| `unbox <T>`             | `obj -> T`                           | 4 | type must match; an `int` converts to `double`; unboxing null is an error unless `T` is nullable |
| `istype <T>`            | `obj -> boolean`                     | 4 | exact runtime type test |
| `tostr.obj`             | `obj -> string`                      | 4 | text of the held value; `null` for null |
| `eq.obj` `ne.obj`       | `obj obj -> boolean`                 | 4 | same type and equal value (reference identity for arrays and maps) |
| `dyn.add` `dyn.sub` `dyn.mul` `dyn.div` `dyn.rem` `dyn.eq` `dyn.ne` `dyn.lt` `dyn.le` `dyn.gt` `dyn.ge` | `obj obj -> obj` | 4 | runtime-validated: numbers (int/double promote; int overflow and division by zero are errors), `+` with a string on either side concatenates, `<` etc. also compare two chars or two strings, `==`/`!=` accept anything (numbers compare by value across int/double, null equals null); other combinations are runtime errors that name both types |
| `dyn.neg` `dyn.not`     | `obj -> obj`                         | 4 | |
| `dyn.index`             | `obj obj -> obj`                     | 4 | arrays (int index) and maps (key converted to the key type) |
| `dyn.setindex`          | `obj obj obj ->`                     | 4 | value converted to the element type |
| `dyn.member "<name>"`   | `obj -> obj`                         | 4 | `length` of arrays, maps, strings |
| `dyn.invoke "<name>" <n>` | `obj arg1..argn -> obj`            | 4 | arrays: `push`, `copy`; maps: `containsKey`, `get`, `remove`, `keys`, `values`; calls with no result return null |
| `pop`                   | `value ->`                           | 1 | |
| `ret`                   | `[value] ->`                         | 1 | the stack must hold exactly the return value (none for `void`) |

`label <name>` (version 2) marks the position of the next instruction; it is not itself an
instruction. Label names are unique within a function and scoped to it. A label may be the target
of `jump`/`jump.false` from anywhere in the same function, before or after it.

`.file "<path>"` (version 5) switches the source file for the following instructions of the same
function (the compiler emits it at the start of every function that comes from an imported
file); `.line` numbers then refer to that file, and runtime errors are reported as
`<path>:<line>`. A function starts in the file named by `source`.

`.line <n>` is a directive, not an instruction: following instructions map to line `n` of the
original `.coy` file until the next `.line`. Runtime errors are reported as `file.coy:<n>`. The
compiler emits `.line` before each instruction that can fail (arithmetic, calls).

## Calling convention

`call` pops the arguments (first argument deepest), starts a new frame whose slots 0..n-1 are
those arguments and whose remaining slots are default-initialized, and runs the callee from its
first instruction. `ret` discards the frame and pushes the result (if any) onto the caller's
stack. Recursion is allowed. Nesting deeper than **5000 frames** is a runtime error
(`call stack overflow`), not a crash. There is no tail-call elimination.

## Host calls and permissions

`hostcall` invokes a host function **only if** all of these hold:

1. the function is registered with the PVM (the registry is compiled into the host program) and
2. the `host` declaration's signature equals the registered signature exactly. Both are checked by
   the verifier, before anything runs;
3. the permission the function requires has been granted to the VM instance. This is checked **at
   the call**: a program may declare host functions it never reaches (a library bundles many)
   without being granted all of them, and a call without the permission is a runtime error that
   happens before the function does anything. The `pvm` CLI grants `console`, `time` and `random`
   by default; more with `--allow <permission>` (`file`, `system`, `net`, `windows`) or `--allow-all`.
   Functions with no permission (`math.*`, `text.*`) are always allowed.

Built-in host functions (the full list with signatures is in [libraries.md](libraries.md)):

| Module | Permission | Functions |
|--------|------------|-----------|
| `con` | `console` | `print`, `println`, `readLine`, `hasInput` |
| `math`, `text` | none | pure functions |
| `time` | `time` | `uptimeMs`, `unixSeconds`, `sleep`, `format` |
| `random` | `random` | `seed`, `nextInt`, `nextDouble` |
| `file`, `dir` | `file` | read/write/copy/move/delete files, list/create/delete folders |
| `env`, `sys` | `system` | environment variables, program arguments, `exit`, working directory |
| `http` | `net` | `request`, `body`, `headers`, `header`, `error` |
| `win` | `windows` | (0.6.3) `messageBox`, `openFileDialog`, `saveFileDialog`, `folderDialog`, `clipboard*`, `reg*`, `process*`, `shellOpen`, `window*`, `notify`; real on Windows, a catchable runtime error elsewhere. Signatures: `stdlib/Standard.Windows/src/*.coy`. |

## Verification

The PVM verifies the whole module before executing anything and reports the first problem as
`<file>:<ml line>: verification error: <message>`. Malformed text is reported by the loader as
`<file>:<ml line>: ml error: <message>`.

Loader (syntax): header and version; unknown directives and instructions; operand shapes;
constructs newer than the declared version; `func` blocks closed by `end`; `local` before the
first instruction; duplicate labels.

Verifier (semantics):

* `source` and `entry` present and unique; entry function exists with `() -> void`;
* no duplicate hosts or functions; every declared host is registered and signature-compatible;
* every `call`, `hostcall`, jump target and `load`/`store` slot refers to something that exists,
  with the right argument count;
* **operand-stack typing across control flow.** The verifier follows every path from the first
  instruction. At every instruction, every path that reaches it must have exactly the same
  operand-stack types (so loops and branches cannot change stack shape), and each instruction
  must find the operand types it requires;
* `ret` leaves exactly the declared return value, and execution cannot fall off the end of a
  function;
* **unreachable code** (no path from the first instruction reaches it) is allowed. It is checked
  for valid names and indices but not for stack typing, and it is never executed.

A program that passes verification cannot underflow the stack, mix types, jump to a missing
label, or run past the end of a function. It can still fail at runtime (arithmetic errors, call
depth) and it can run forever: **there is no instruction or time limit**.

## Memory management

Arrays live on a managed heap. The PVM uses a precise, non-moving, stop-the-world mark-and-sweep
collector. Roots are the operand stack and the slots of every active frame. Roots also include globals. Collection happens only
inside an allocating instruction (`newobj`, `newarray`, `arraylit`, `acopy`, `apush`); the operands of that
instruction stay on the stack, so they remain reachable. Types prevent cycles from being created,
but the collector does not depend on that. A collection starts when the object count or element
count exceeds the larger of a floor (1024 objects / 2^20 elements) and twice the live amount after
the previous collection.

The heap is bounded by `--heap-limit` live elements (default 8,000,000). Exceeding it after a
collection is the runtime error `out of memory: ...`, not a crash. Strings are copied by value and
are not on the heap. Native resources (files, sockets) will need explicit lifetimes; none exist yet.

`--gc-stress` collects before every allocation (testing); `--gc-stats` prints collection counts.

## Resource limits

Call depth 5000. ML files over 32 MiB are rejected by the `pvm` CLI (exit 2) because the loader
keeps every instruction in memory. The verifier's memory is linear in program size.

## Exceptions (version 8)

Without a handler, a failing instruction, a host function or `throw` aborts execution with
`<file.coy>:<line>: runtime error: ...` and exit code 1. Version 8 adds handlers:

| Instruction | Stack | Meaning |
|---|---|---|
| `try <label>` | `->` | the operand stack must be empty; registers `<label>` as the handler of the current function until the matching `endtry` |
| `endtry` | `->` | removes the innermost handler of the current function (every normal exit from a `try` region executes one) |
| `throw` | `string ->` | raises a runtime error whose message is the string |

When an error is raised, the PVM looks for the innermost active handler, starting in the current function and then in its callers (functions
above are abandoned; a function's handlers die with it). The handler's function resumes at the label with its operand stack cleared
and the error message (`string`) pushed. A handler is removed when it is used. The verifier checks that the label exists, that the stack is empty at `try`, and
treats the label as reachable with a one-element stack `[string]`. Locals keep their values. Out-of-memory (heap) errors cannot be handled.

## Example

```
method fib(int n) : int {          ml 2
    if (n < 2) { return n }        source "fib.coy"
    return fib(n - 1) + fib(n - 2) func fib (int) -> int
}                                    load 0
                                     push.int 2
                                   .line 2
                                     lt.int
                                     jump.false L0
                                     load 0
                                     ret
                                   label L0
                                     ...
```

(Run `coyote build samples/fibonacci.coy -o -` to see the full output.)

## Planned (not implemented)

Type metadata and an optional
instruction budget.

## Version 7: class hierarchies

`class D extends A B ...` declares supertypes (a base class and/or interfaces; interfaces are classes without fields). A class must start with the fields of each supertype it extends (same names, types and order) and the hierarchy must be acyclic; the verifier checks both. Three instructions work with hierarchies:

| Instruction | Stack | Meaning |
|---|---|---|
| `upcast C` | `D -> C` | statically checked: `D` must be `C` or a subtype; no runtime cost |
| `downcast C` | `B -> C` | `C` must be a subtype of `B`; at run time the object must be a `C` (or a subtype), otherwise `invalid cast`; null stays null |
| `isinst C` | `B -> boolean` | true when the object is a `C` or a subtype (null gives false) |

Method dispatch is compiled by the Coyote compiler into `isinst` chains; the PVM has no virtual-call instruction.

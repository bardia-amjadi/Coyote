# The compiler (`coyote`)

`coyote` turns `.coy` source into textual ML (see [ml-spec.md](ml-spec.md)); the PVM, the debugger and the
native backend all consume that ML.

```
coyote build app.coy -o app.ml      compile ('-o -' writes the ML to stdout)
coyote check app.coy                report errors only
coyote run app.coy [-- args]        compile to a temporary file and run it on the PVM with every permission
coyote lib pack <dir> [-o X.lib]    build a library (see libraries.md)
coyote llvm app.ml -o app           native backend (see llvm.md)
```

Options for `build` and `run`: `--keep-unused` (see below).

## Passes

1. **Lex / parse** each file (`import` and `use` pull in more files and libraries).
2. **Generic expansion** (`generics.cpp`): every `Box<int>` / `Id<int>(x)` becomes its own generated class or method;
   generic declarations are removed. See *Generics* in [language.md](language.md).
3. **Semantic analysis** (`sema.cpp`): name and type checking, class hierarchies (interfaces, base classes, overrides,
   `base(...)`), implicit conversions (`int -> double`, `T -> T?`, subclass -> base class), constructors, properties.
   Virtual calls are resolved here: a call through a class or interface type is bound to a synthesized *dispatcher*
   when some subclass can override the method.
4. **ML generation** (`mlgen.cpp`): textual ML, version 5 normally, version 7 when the program uses inheritance or
   interfaces. Dispatchers become chains of `isinst` tests, most derived class first.

## Dead-code removal

Methods that cannot be reached from the program's top-level code (directly or through other reachable methods,
constructors, static initializers and dispatchers) are **not written to the ML**. This keeps programs small when they
`use "Standard"`: only the library methods you actually call are compiled in. The check is purely static, so nothing
reachable is ever removed. Errors in unused methods are still reported (type checking runs before removal).

`--keep-unused` disables the removal, e.g. to inspect all of a library's methods in the generated ML. Unused
classes are kept (they cost a few lines) and so are `hand` declarations; permissions are only checked when a host
function is actually called.

## Diagnostics

Errors are printed as `file:line:col: error: message`, sorted by file and position, followed by `N error(s)` when
there are several. One mistake normally produces one error: expressions that already failed are typed `<error>` and
produce no follow-up diagnostics.

# Coyote language — what is implemented today (0.6)

This page describes only what the current compiler accepts. Everything else in the language
design is **rejected with an error**, never silently ignored. Syntax marked *provisional* is a
working choice that may change when the language design is finalized.

## Source files

`.coy` files, UTF-8. `#` starts a comment that runs to the end of the line. Identifiers are
case-sensitive (`[A-Za-z_][A-Za-z0-9_]*`). Blocks use curly braces. Semicolons are optional: a
statement ends at a newline, at `;`, or at a closing `}`. Two statements on one line must be
separated by `;`.

## Program shape

A file is a sequence of `hand` declarations, `method` declarations and statements. Statements at
the top level run in order, top to bottom, as the program's entry point. `hand` and `method`
declarations are visible in the whole file, including above their declaration.

```
hand:con println(string text)

method greet(string name) {
    println("Hello, " + name + "!")
}

greet("Coyote")
```

## Types

| Type      | Values |
|-----------|--------|
| `int`     | 32-bit signed integer. Arithmetic that leaves the range is a **runtime error** (`integer overflow`), never silent wraparound. |
| `string`  | immutable text |
| `boolean` | `true`, `false` |
| `double`  | IEEE-754 binary64: `1.5`, `2.0e3` (a digit must follow the `.`) |
| `char`    | one Unicode code point: `'a'`, `'\n'`, `'é'`; escapes `\n \t \r \0 \\ \' \"` |
| `Guid`    | UUID; created with `Guid("...")`, printed as lowercase `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx` |
| `array<T>` | dynamic array of `T` (any of these types, including `array<array<int>>`); reference semantics |
| `map<K,V>` | insertion-ordered map; `K` is `int`, `string`, `char`, `boolean` or `Guid` |
| `T?`      | `T` or `null` (e.g. `int?`, `string?`, `array<int>?`) |
| `obj`     | holds a value of any type; the held type may change on each assignment (like C# `object`) |
| `dyn`     | like `obj`, but operators, indexing, members and method calls are validated at run time |

### Conversions

Implicit (the compiler inserts them): `int` → `double`; `T` → `T?`; `null` → `T?`/`obj`/`dyn`;
anything → `obj`/`dyn` (boxing); `dyn` → any type (checked at run time); `obj` ↔ `dyn`.
Everything else is explicit and errors tell you which one is missing:

| Written | Meaning |
|---------|---------|
| `int(x)` | from `double` (truncates; out of range is a runtime error), `char` (code point), `string` (strict parse), `obj`/`dyn` |
| `double(x)` | from `int`, `char`, `string`, `obj`/`dyn` |
| `string(x)` | from `int`, `boolean`, `double`, `char`, `Guid`, `obj`/`dyn` |
| `char(x)` | from `int` (must be a valid code point), `obj`/`dyn` |
| `Guid(x)` | from `string` (strict parse, any case), `obj`/`dyn` |

`+` with a string on either side converts the other operand (`int`, `boolean`, `double`, `char`,
`Guid`, `obj`) to text. `var` infers the type from the initializer (`var x = null` is an error).

### Null

Only `T?`, `obj` and `dyn` can be null. `x == null` / `x != null` test it (comparing any other
nullable values with `==` is an error: compare with null or unwrap). `a ?? b` yields `a` unless it is null
(right-associative; `b` may itself be nullable). Postfix `x!` unwrap: null is a runtime error.
`v is T` is true when the `obj`/`dyn` value `v` currently holds exactly a `T`.

## Variables

```
var count = 3          # type inferred
int total = 0          # explicit type
string s = "x"
boolean ok = count > 2
count = count + 1      # assignment
```

* Every variable needs an initializer.
* Scope is the enclosing `{ }` block. Declaring a name that is already visible in an enclosing
  scope of the same method (or top-level code) is an error; sibling blocks may reuse names.
* Top-level variables are **not** visible inside methods (there are no globals yet); pass values as
  parameters.

## Expressions

Precedence, lowest to highest (all binary operators are left-associative):

| Level | Operators | Operand types → result |
|-------|-----------|------------------------|
| 1 | `\|\|` | boolean, boolean → boolean (short-circuit) |
| 2 | `&&` | boolean, boolean → boolean (short-circuit) |
| 3 | `==` `!=` | two operands of the same type → boolean; with `null`; `obj` vs anything (boxed) |
| 4 | `<` `<=` `>` `>=`, `is` | int/double (mixed promotes to double), char; `v is T` → boolean |
| 5 | `+` `-` | numbers (int,int → int; otherwise double); for `+`, if either side is a string the result is a string |
| 6 | `*` `/` `%` | numbers; `%` on doubles is `fmod` |
| 7 | unary `-` `!`, postfix `!` | number → same; boolean → boolean; `T?` → `T` (postfix) |

`??` has the lowest precedence (below `\|\|`). If either operand of an arithmetic, comparison or
equality operator is `dyn`, the operation runs at run time and the result is `dyn`; `dyn` is
accepted wherever a condition or any other type is required (checked at run time).

`/` truncates toward zero and `%` takes the sign of the dividend (`-7 / 2 == -3`,
`-7 % 3 == -1`). Division or remainder by zero is a runtime error. `-2147483648` cannot be written
as a literal (the literal `2147483648` is out of range); compute it as `0 - 2147483647 - 1`.

Literals: integers (decimal, `0` to `2147483647`), doubles, characters, `null`, strings (`"..."` with escapes `\n \t \r \\ \"`,
single line), `true`, `false`.

### Interpolated strings

```
string name = "Coyote"
println($"Hello {name}, 2 + 3 = {2 + 3}, braces: {{ok}}")    # Hello Coyote, 2 + 3 = 5, braces: {ok}
```

`$"..."` is a normal string in which each `{expression}` is converted to text and joined (`"Hello " + name + ...`); `{{` and `}}` are literal braces.
The expression may be any expression that does not contain a double quote (the string would end there). Plain strings without `$` never treat `{` specially.

## Arrays

```
array<int> nums = [5, 3, 8]        # literal; an empty literal [] is an error (no type to infer)
var grid = new array<array<int>>(3) # 3 slots, each a null array until assigned
array<string> names = new array<string>(2)   # elements default to "" (0, false)
nums[0] = nums[1] * 2              # index read/write
nums.push(1)                       # append (statement)
println("n = " + nums.length)      # length (int)
var dup = nums.copy()              # shallow copy
var alias = nums                   # same array: alias[0] = 9 changes nums too
```

* `==` and `!=` on arrays compare **identity**, not contents.
* Out-of-range or negative indexes, a negative `new` size, and using an unassigned (null) nested
  array are runtime errors reported at the `.coy` line. Exceeding the heap limit is
  `out of memory`.
* `[`, `.` after an expression must be on the same line as the expression.
* Arrays are garbage collected (see `architecture.md`).

## Namespaces, `use` and `export`

```
# src/models/point.coy
namespace MyApp.Models                 # must be the first line of the file (comments may come before it)
export model Point {
    int x
    int y
}

# src/main.coy  (the entry file)
namespace MyApp
use "Standard"                         # a library
use "MyApp.Models"                     # a namespace of this project: you now see its exports
var p = new Point(3, 4)
```

* **`namespace Name`** puts every declaration of the file into that namespace. Names are dotted (`MyApp.Models`); `MyApp.Models`
  is a separate namespace from `MyApp` (there is no nesting or inheritance between them). A file without a `namespace` line is in no
  namespace; the entry file may do without one.
* **`use "Name"`** is the only way to see another file's code. `Name` is one of three things, looked up in this order of kinds:
  1. a **namespace of the project**: every `.coy` file under the project folder (the folder with the `.cyproj` / `packages.json`, otherwise
     the folder of the file) whose first declaration is `namespace Name`. Folders called `bin`, `obj`, `dist`, `node_modules` and
     folders starting with `.` are not searched (at most 5000 files);
  2. an **installed package** (`.coyote/packages/<name>/` next to the file or any folder above it; run `cpm install`);
  3. a **library** (`use "Standard"`, see [libraries](libraries.md)); `use "./My.lib"` names a library file directly.

  If a name matches more than one kind, `use` reports it as ambiguous. `use` accepts only quoted names, never a `.coy` path.
* **The files of one namespace see each other's exports without `use`**: a file declaring `namespace MyApp.Models` sees the exports of
  every other file of `MyApp.Models`. Only the entry file may contain top-level statements; another program's statements in a file
  that is merely a same-namespace neighbour are ignored (a file named by `use` that has top-level statements is an error).
* **`export`** controls what other namespaces see: `export method`, `export class`, `export model`, `export interface`. A method that is not
  exported is private to its file. Private methods of different files may share a name.
* `use` and `namespace` appear only at the top level. `use` is not transitive: a file sees the exports of what *it* uses (and of its own
  namespace), not of what those files use. Two uses that export the same name, or a use that clashes with a local declaration, is an error.
* **`import` was removed in 0.6.** Replace `import "lib/math.coy"` by `namespace Lib` at the top of that file and `use "Lib"`;
  the compiler's message says so.
* Errors and runtime errors in another file are reported with that file's path.
* Not implemented: qualified names (`MyApp.Models.Point`), `use` with an alias or a list of names, namespaces that contain other namespaces.

## Maps

```
map<string, int> ages = new map<string, int>()    # maps are created empty
ages["ann"] = 30                  # insert or replace
println("" + ages["ann"])         # missing key is a runtime error
ages.containsKey("bob")           # boolean
ages.remove("ann")                # boolean: did it exist
int? maybe = ages.tryGet("zed")   # null when missing
ages.length                       # number of entries
for (var k in ages.keys()) { }    # keys()/values() return arrays in insertion order
```

## Obj and dyn

```
obj o = 42            # any value
o = "now a string"    # the held type can change
if (o is string) { println(string(o)) }
int n = int(o)        # explicit conversion; wrong type is a runtime error

dyn d = [1, 2, 3]
d[1] = 20             # runtime-validated indexing, d.length, d.push(4), d + 1, ...
```

`obj` supports only `==`/`!=`, `is`, null checks and explicit conversions. `dyn` supports the
dynamic operators; its results are `dyn`. Errors name the runtime types involved
(`operator '-' cannot be applied to string and int`).

## For-each

`for (var x in array) { }` or `for (T x in array) { }` iterates an array (the array expression
is evaluated once; the loop reads the live length). `break` and `continue` work. A map must be
iterated through `.keys()` or `.values()`.

## Classes and models

```
class Counter {
    static int made = 0           # static field, one per class (initialized before the program runs)
    int count = 10                # instance field with an initializer
    private string label          # private: only code of this class may use it
    property int Doubled { get { return this.count * 2 } }   # computed, read-only property
    property int Step             # auto-property (hidden backing field, public get and set)

    constructor(string label, int step) { this.label = label; this.Step = step; made = made + 1 }
    method bump() { count += Step }
    static method total(): int { return made }
}
model Point {                      # 'model': a constructor taking every field in order is generated
    int x
    int y
}
Counter c = new Counter("a", 3)
c.bump()
```

* A class instance is a reference: `Counter b = a` makes both names refer to one object (copy fields explicitly).
* Fields default to `0`, `""`, `false`, `null`; a field of class type must therefore be nullable (`Node? next`) to start empty.
  Field initializers run, in declaration order, at the start of the constructor.
* Inside instance methods bare names resolve to locals first, then fields/methods of the class (`this.x` is always allowed);
  static methods cannot use instance members.
* A class has at most one constructor (no overloading); without one, a class gets a no-argument constructor and a model gets the all-fields one.
  `return` is not allowed in a constructor. Constructors, methods, fields and properties can be `public` (default) or `private`.
* `export class` / `export model` make a class visible to the files that `use` its namespace; it can be named as a type there.
* Compound assignment (`+=` ...) works on variables and on fields (`x.count += 1`, `count += Step`).
* Inheritance and interfaces: see the next section. Not implemented: overloading, static properties, operators on objects, `==` structural comparison
  (`==` on two objects compares identity), object printing (`string(obj)` is an error).

## Inheritance and interfaces

```
interface Shape {                       # signatures only: no fields, no bodies
    method Area(): double
    method Name(): string
}
interface Solid : Shape { method Volume(): double }    # an interface can extend interfaces

class Animal {
    string name
    protected int legs = 4              # visible in Animal and its subclasses
    constructor(string n) { name = n }
    method Speak(): string { return "..." }
    method Describe(): string { return name + " says " + Speak() }   # Speak() is virtual
}
class Dog : Animal {                    # base class first, then interfaces: class D : Base, I1, I2
    constructor(string n) : base(n) { } # runs Animal's constructor body first
    override method Speak(): string { return "Woof" }
}
class Puppy : Dog {
    constructor(string n) : base(n) { }
    override method Speak(): string { return base.Speak() + "!" }    # base.m(...) calls the base version
}
Animal a = new Puppy("Bit")             # a subclass value can be used wherever its base class or interface is expected
println(a.Describe())                   # Bit says Woof!
if (a is Dog) { Dog d = a as Dog }      # 'is' tests the real class; 'as' is a checked downcast
```

* A class has **one** base class and any number of interfaces. A subclass inherits fields, properties and methods.
  Every instance method is virtual: a call through `Animal` runs the override of the object's real class.
* `override` is optional, but when written it must replace an inherited method (otherwise it is an error). An override
  must have exactly the same parameter types and return type. Fields cannot be redeclared (no hiding).
* A class must implement every interface method (itself or through its base class), otherwise
  `class 'C' must implement 'M(): int' required by 'I'`. Interfaces cannot be instantiated.
* `constructor(...) : base(args)` calls the base constructor first; if omitted, `base()` is called implicitly (so the base
  needs a constructor that takes no arguments). Field initializers of each class run before its own constructor body.
* `private` members are visible only inside their own class; `protected` also in subclasses.
* `x as T` converts a class value to `T`: an upcast is always fine, a downcast fails at run time with `invalid cast` when
  the object is not a `T`. `x is T` is true for `T` or any subclass. Unrelated classes are a compile-time error.
* `a == b` between objects still compares identity; arrays and `T?` are not covariant (`array<Dog>` is not an `array<Animal>`).
* How it works: programs that use inheritance are emitted as ML version 7 (`class D extends A I`, `upcast`, `downcast`,
  `isinst`, see ml-spec.md). The compiler synthesizes a dispatcher `Class.m$v` per overridable method (a chain of `isinst`
  tests, most derived class first), so the PVM needs no virtual-call instruction. Programs without hierarchies still
  emit ML 5.

## Generics

```
class Box<T> {                              # a generic class: T is replaced by the type you give
    T value
    constructor(T v) { value = v }
    method Get(): T { return value }
}
class Pair<K, V> {
    K key
    V val
    constructor(K k, V v) { key = k; val = v }
    method Swap(): Pair<V, K> { return new Pair<V, K>(val, key) }
}
class Node<T> { T item; Node<T>? next }     # recursive generics work (a field of its own type must be nullable)
interface Source<T> { method Next(): T }    # generic interfaces too: class Counter : Source<int>

method First<T>(array<T> xs): T { return xs[0] }     # a generic top-level method
Box<int> b = new Box<int>(41)
Pair<string, int> p = new Pair<string, int>("a", 1)
Box<Box<int>> nested = new Box<Box<int>>(new Box<int>(5))
println("" + First<int>([7, 8]))                     # generic methods need their type arguments: First<int>(...)
```

* Generics are expanded at compile time (monomorphization): `Box<int>` and `Box<string>` are two separate classes
  generated from the same source, internally named `Box$int$` and `Box$string$`. There is no run-time cost and the PVM,
  the ML format and the native backend need no generics support. Error messages show the readable `Box<int>` form.
* `Box<int>` and `Box<string>` are unrelated types: a `Box<string>` cannot be assigned to a `Box<int>`.
* A type parameter can be any type (`int`, `string`, `array<int>`, another generic, a class...). There are no constraints:
  the generic code is checked after substitution, so `a.Foo()` on a `T` is accepted exactly when the argument type has a `Foo`.
* Type arguments are always written explicitly. Not supported: inferring `First([1, 2])`, generic methods inside
  classes (make the class generic instead), generic base classes with type parameters of the derived class used as
  a different shape, and `new T()`.
* Using a generic class without arguments, the wrong number of arguments, or arguments on a non-generic class is an error.

## Variadic parameters and extension methods

```
method sum(int... xs): int { ... }          # xs is an array<int> inside the method
sum()  sum(1)  sum(1, 2, 3)                 # extra arguments are packed into an array
sum(existing)                               # or pass one array<int> directly
method join(string sep, string... parts): string { ... }

method shout(this string s): string { return s + "!" }   # extension method
"hi".shout()                                # same as shout("hi")
method total(this array<int> a): int { return sum(a) }
```

* A variadic parameter must be the last one; at most one. `hand:` declarations cannot be variadic.
  Passing exactly one argument that already has the array type passes the array itself (no wrapping).
  Methods, class methods and constructors can be variadic.
* An extension method is a top-level method whose first parameter is marked `this`. `recv.name(...)`
  uses it only when the receiver has no built-in method (`push`, `copy`, map methods...) or class method
  of that name, so extensions cannot override those. It is also callable as a plain function. Extensions
  from another file need `export method` and a `use` of its namespace, like any method. Class members cannot be extensions,
  and extensions do not apply to `dyn` receivers (their calls are resolved at run time).
* Both are compile-time rewrites: the ML and the PVM are unchanged.

## Limits

Source files over 4 MiB (exit code 2), nesting deeper than 256 blocks/parentheses, and expressions
deeper than 1000 levels are rejected with a diagnostic.

## Statements

```
if (cond) { ... } else if (cond) { ... } else { ... }
while (cond) { ... }
for (init; cond; step) { ... }   # each part optional; no cond means true
break             # leave the innermost loop
continue          # next iteration (a 'for' runs its step first)
return            # in a void method
return value      # in a method with a return type
{ ... }           # a block
name = value      # assignment
name += value     # also -= *= /= %=; works on variables, fields and array elements (x.n += 1, a[i] *= 2)
name++            # also name-- ; a statement (and a 'for' step), not an expression
# += and ++ need a target without side effects: a[i] and a[i + 1] are fine, a[next()] is not (write the full assignment)
call(args)        # call used as a statement; a returned value is discarded
```

Conditions must be `boolean` and need parentheses. Braces are required around bodies.

In `for (init; cond; step)`, `init` is a declaration (`var i = 0`, `int i = 0`) or an assignment,
and `step` is an assignment or a call. A variable declared in `init` is visible only in the loop.
`x op= v` means `x = x op v`, with the same typing and runtime errors as the plain operator.
`break`/`continue` outside a loop are compile errors; they never cross a method boundary.

## Methods *(provisional syntax)*

```
method name(int a, string b) : int {
    return a
}
method log(string text) { ... }      # no ': type' means void
```

* Declared at the top level only. Parameters and return values may be any type (`int`, `string`, `double`, `char`, arrays, maps, nullable types, classes...).
* Recursion and mutual recursion work. Nesting deeper than 5000 calls is a runtime error.
* A method with a return type must return a value on **every** code path (the compiler checks
  `return`, and `if`/`else` where both branches return; a `while` never counts). Otherwise:
  `method 'f' does not return a value on all code paths`.

## `hand:<module> <name>(<type> <param>, ...)`

Declares a host intrinsic: a function implemented by the PVM, not by Coyote code. Parameter types
are any value type (the PVM's registered functions currently take `string`, `int` or `boolean`); host functions may declare a return type (`hand:math sqrt(double x): double`; see [libraries](libraries.md)). A declaration only **asks** for a
host function; the PVM runs it only if it is registered and permitted (see `ml-spec.md`).

## Reserved but not implemented

Using any of these gives `'<word>' is not supported yet` (and they cannot be used as names):

`hidden created here operator module`

`import` was removed in 0.6 (use `namespace` + `use`); `namespace` is a keyword now.

Also not implemented: `else` without braces, negative literals
in `hand` calls (use an expression), top-level globals (use `static` class fields), method overloading, static properties, closures/lambdas, map literals, iterating strings or maps directly,
member assignment on `dyn` (`d.x = 1`), `dyn` method calls beyond the built-in array/map ones.

## Design decisions recorded so far

* **`obj` and `dyn`.** `obj` holds values of different runtime types over its lifetime (like C#
  `object`); `dyn` additionally allows dynamic operations validated at runtime. They share one ML
  type (`obj`); the compiler decides statically whether an operator is a static one or a `dyn.*`
  instruction.
* **Doubles never raise arithmetic errors** (IEEE); `int(x)` of NaN/infinity/out-of-range does.
* **No silent integer wraparound.** `int` overflow is an error, so results are never quietly wrong.
* **Host access is explicit.** `hand` declarations never grant native access (see ML spec).
* **Errors are deterministic**: `file:line:col: error: message`, sorted by position, no
  environment-dependent text. The parser reports several syntax errors per run: after an error it skips to the end of the broken
  statement (or declaration) and carries on, at most 50 errors, and never two on the same line; semantic analysis
  reports all errors it finds, without cascades from an earlier mistake. (Semantic analysis only runs when there are no syntax errors.)

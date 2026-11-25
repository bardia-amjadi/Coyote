# Coyote language — what is implemented today (0.7.0)

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
| `long`    | 64-bit signed integer (0.7). Literals: `5L`, or any decimal/hex/binary literal that does not fit an `int`. Overflow is a runtime error. |
| `byte`    | unsigned 8-bit integer 0..255 (0.7). Meant for storage and files: `array<byte>` keeps **one byte per item**. Arithmetic on bytes is done in `int`. |
| `string`  | immutable text |
| `boolean` | `true`, `false` |
| `double`  | IEEE-754 binary64: `1.5`, `2.0e3` (a digit must follow the `.`) |
| `char`    | one Unicode code point: `'a'`, `'\n'`, `'é'`; escapes `\n \t \r \0 \\ \' \"` |
| `Guid`    | UUID; created with `Guid("...")`, printed as lowercase `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx` |
| `array<T>` | dynamic array of `T` (any of these types, including `array<array<int>>`); reference semantics |
| `map<K,V>` | insertion-ordered map; `K` is `int`, `long`, `byte`, `string`, `char`, `boolean` or `Guid` |
| `T?`      | `T` or `null` (e.g. `int?`, `string?`, `array<int>?`) |
| `obj`     | holds a value of any type; the held type may change on each assignment (like C# `object`) |
| `dyn`     | like `obj`, but operators, indexing, members and method calls are validated at run time |

### Conversions

Implicit (the compiler inserts them): the widening chain `byte` → `int` → `long` → `double` (an `int` literal 0..255 also converts to `byte`); `T` → `T?`; `null` → `T?`/`obj`/`dyn`;
anything → `obj`/`dyn` (boxing); `dyn` → any type (checked at run time); `obj` ↔ `dyn`.
Everything else is explicit and errors tell you which one is missing:

| Written | Meaning |
|---------|---------|
| `int(x)` | from `double` (truncates; out of range is a runtime error), `long` (error when it does not fit), `byte`, `char` (code point), `string` (strict parse), `obj`/`dyn` |
| `long(x)` | from `int`, `byte`, `double` (truncates; range checked), `char`, `string` (strict parse), `obj`/`dyn` |
| `byte(x)` | from `int`, `long`, `char`; a value outside 0..255 is a runtime error |
| `double(x)` | from `int`, `long`, `byte`, `char`, `string`, `obj`/`dyn` |
| `string(x)` | from `int`, `long`, `byte`, `boolean`, `double`, `char`, `Guid`, `obj`/`dyn` |
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
| 3 | `\|` | int/long (bitwise or) |
| 4 | `^` | int/long (bitwise xor) |
| 5 | `&` | int/long (bitwise and) |
| 6 | `==` `!=` | two operands of the same type → boolean; with `null`; `obj` vs anything (boxed) |
| 7 | `<` `<=` `>` `>=`, `is` | numbers (the smaller type widens), char, **string** (by code point); `v is T` → boolean |
| 8 | `<<` `>>` | int/long shifted by an `int` count (taken modulo 32 / 64); `>>` is arithmetic |
| 9 | `+` `-` | numbers (the result has the widest operand type, at least `int`; `byte` computes as `int`); for `+`, if either side is a string the result is a string |
| 10 | `*` `/` `%` | numbers; `%` on doubles is `fmod` |
| 11 | unary `-` `!` `~`, postfix `!` | number → same (`byte` → `int`); boolean → boolean; `~` int/long; `T?` → `T` (postfix) |

Bitwise and shift operators need integral operands (`int`, `long`, `byte`); `a & b` of two `byte` values is an `int` (convert back with `byte(x)`). `>>` is written as two adjacent `>` so that
`List<List<int>>` still parses; `>>=` works the same way. Compound forms: `&= |= ^= <<= >>=`. Integer literals: `42`, `5L`, `0xFF`, `0b1010` (an `int` when it fits 31 bits, else a `long`; the smallest values cannot be written as a literal, write
`-2147483647 - 1` or `-9223372036854775807L - 1`).

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

## Namespaces, `use` and `public`

```
# src/models/point.coy
namespace MyApp.Models                 # must be the first line of the file (comments may come before it)
public model Point {
    field int x
    field int y
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
* **`public`** controls what other namespaces see: `public method`, `public class`, `public model`, `public interface` (`export` was
  replaced by `public` in 0.6.3; the old word gives an error that says so). A top-level declaration without `public` is private to its
  file. Private methods of different files may share a name.
* **The entry file may declare classes, models, interfaces and methods itself** and use other namespaces at once (`use "MyApp.Models"`), see
  *Classes in the entry file* below.
* `use` and `namespace` appear only at the top level. `use` is not transitive: a file sees the exports of what *it* uses (and of its own
  namespace), not of what those files use. Two uses that export the same class name, or a use that clashes with a local declaration, is an error (two exported methods of the same name are allowed; call them with their namespace).
* **`import` was removed in 0.6.** Replace `import "lib/math.coy"` by `namespace Lib` at the top of that file and `use "Lib"`;
  the compiler's message says so.
* Errors and runtime errors in another file are reported with that file's path.
* Qualified names (`MyApp.Models.Point`) work since 0.8, see below. There is no `use` with an alias or a list of names and no namespace that contains other namespaces (not planned for 1.0).

## Maps

```
map<string, int> ages = new map<string, int>()    # maps are created empty
ages["ann"] = 30                  # insert or replace
println("" + ages["ann"])         # missing key is a runtime error
ages.containsKey("bob")           # boolean
ages.remove("ann")                # boolean: did it exist
int? maybe = ages.tryGet("zed")   # null when missing
ages.length                       # number of entries
in (var k of ages.keys()) { }    # keys()/values() return arrays in insertion order
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

## Strings as sequences (0.7)

`s[i]` is the `char` (code point) at index `i`; `s.length` is the number of code points (so `"€".length` is 1); `in (var c of s) { }` visits every code point; `<`, `<=`, `>`, `>=` order strings by
code point. A string is immutable: `s[i] = x` is a compile error. `Length()` and `CharAt` of Standard.Text keep working. Indexing is O(n) for non-ASCII text.

## Bytes (0.7)

`array<byte>` stores one byte per item (an `array<int>` uses a full value per item). `new array<byte>(n)`, `[1, 2, 255]`, `b[i]`, `b[i] = 7`, `b.push(x)`, `b.length`, `(copy)b` and `in (var x of b)` work as for other
arrays; items are `byte`. `File.ReadAllBytes(path)`, `File.WriteAllBytes(path, data)`, `File.AppendBytes`, `Encoding.ToBytes(text)` (UTF-8) and `Encoding.FromBytes(data)` (invalid sequences become U+FFFD)
use it. Limit: an `array<byte>` cannot be indexed or called through `dyn` (assign it to an `array<byte>` variable first); the older `Utf8`/`Hex`/`Base64` classes of Standard.Encoding still use `array<int>`
(`Encoding.IntsToBytes` / `BytesToInts` convert).

## Enums (0.7)

```coyote
enum Color { Red, Green = 5, Blue }       # Blue is 6; values are int literals
var c = Color.Green
println(c.ToString() + " " + c.Value)     # Green 5
Color.Parse("Blue")  Color.TryParse("x")  # Color / Color? ; Parse stops the program on an unknown name
Color.FromValue(5)   Color.Values()       # Color / array<Color>
c.CompareTo(Color.Red)                    # by value; == compares members by identity
```

An enum is a class of shared, read-only instances with `Name`, `Value`, `ToString()`, `CompareTo(other)` (the compiler generates it; names `Name Value ToString CompareTo Parse TryParse FromValue Values all` cannot be members).
Enums carry no data of their own and cannot be extended. `public enum` exports it like a class.

## `switch` (0.7)

```coyote
switch (n) {                      # statement: no fall-through, 'break' still belongs to an enclosing loop
    case 1: println("one")
    case 2, 3:
        println("two or three")
    default: println("other")
}
var name = switch (c) { Color.Red => "r", Color.Green => "g", Color.Blue => "b" }   # expression
var size = switch (n) { 1 => 10, 2, 3 => 20, _ => 0 }
```

Case values are compared with `==` (ints, strings, chars, enum members, ...). The **expression** must give every arm one type (numbers widen) and is checked for completeness: over an enum it must list every member or
have a `_` arm; over any other type it needs `_`. The statement needs neither a `default` nor completeness.

## For-each

`in (var x of array) { }` or `in (T x of array) { }` (0.6.3; the old `for (var x in array)` is an error that shows the new form) iterates an array (the array expression
is evaluated once; the loop reads the live length). `break` and `continue` work. A map must be
iterated through `.keys()` or `.values()`; `in (var k of map)` is shorthand for `in (var k of map.keys())`.

## Classes and models

```
class Counter {
    shared field int made = 0           # shared field, one per class (initialized before the program runs)
    field int count = 10                # instance field with an initializer
    hidden field string label          # hidden: only code of this class may use it
    property int Doubled { get { return here.count * 2 } }   # computed, read-only property
    property int Step             # auto-property (hidden backing field, public get and set)

    constructor(string label, int step) { here.label = label; here.Step = step; made = made + 1 }
    method bump() { count += Step }
    shared method total(): int { return made }
}
model Point {                      # 'model': a constructor taking every field in order is generated
    field int x
    field int y
}
Counter c = new Counter("a", 3)
c.bump()
```

Since 0.6.3 every member says what it is: `field` or `property` comes before the type (`model` bodies too), and modifiers come first.
`private`, `static` and `this` became `hidden`, `shared` and `here`; the old words are reserved and the compiler answers with the new name.

```
public class Tools {
    public shared locked field int Max = 10          # constant: only its initializer may set it
    public readonly field string Name = "tools"      # set by the initializer or in this class's constructor only
    hidden field int calls = 0                       # hidden = only this class (was private)
    public field Title = "hello"                     # typeless: the type comes from the literal (int, double, string, char, boolean)
    public property string Label { get; set; } = "x" # auto property with an initial value
    public property int Id { get; } = 7              # get-only auto property: only its initializer sets it
    public method GiveSpecialNumber(): int { calls += 1; return Max * 4 + 2 }
}
Shell.PrintLine(new Tools().GiveSpecialNumber())     # 'new' can be used inside a larger expression
```

* **Modifiers** (any order before `field`/`property`/`method`/`constructor`): `public` (default for members), `hidden`, `protected`, `shared`,
  `override`, `locked`, `readonly`. `locked` and `readonly` apply to fields only.
* **`locked`** is a constant. Its initializer must be a constant expression (literals, operators, conversions, other `locked` fields) and
  it can never be assigned. **`readonly`** can be set by its initializer, or (instance fields) in a constructor of the class that declares it.
* **`shared`** members belong to the class. **`shared class X { ... }`** makes every member shared; such a class cannot be instantiated with `new`.
* **`public class`**, `public model`, `public interface` and `public method` at the top level are visible to other namespaces (replaces `export`).
* `here` is the current object (replaces `this`); `here T name` marks an extension method's receiver.
* **Constructors can be overloaded** (0.6.5), like methods: `constructor()`, `constructor(int a)`, `constructor(string t)`. `new P(3)` picks the
  best fit (exact match, then widening such as int to double; ambiguity is an error that lists the candidates). Two constructors with the same
  parameter types are an error. Each constructor gets the field initializers; in a class hierarchy `: base(args)` picks the base class's
  constructor the same way (a constructor without `: base(...)` needs a base constructor that takes no arguments). A constructor cannot call another
  constructor of its own class yet: put shared code in a hidden method.
* Mixing in the old words gives, e.g. `'private' was renamed to 'hidden'`.
* **Migrating old code:** `python3 tools/migrate-0.6.3.py file.coy ...` rewrites the keywords, adds `field`, and converts `for (x in xs)` loops in place (class bodies written on one line must be edited by hand).

### Classes in the entry file

The entry file (the one with top-level statements) may declare classes, models, interfaces and methods and `use` other namespaces:

```
namespace MyApp
use "Standard"
use "MyApp.Models"

class Local { public method Hi(): string { return "hi" } }
Shell.PrintLine(new Local().Hi())
```

* A class instance is a reference: `Counter b = a` makes both names refer to one object (copy fields explicitly).
* Fields default to `0`, `""`, `false`, `null`; a field of class type must therefore be nullable (`Node? next`) to start empty.
  Field initializers run, in declaration order, at the start of the constructor.
* Inside instance methods bare names resolve to locals first, then fields/methods of the class (`here.x` is always allowed);
  shared methods cannot use instance members.
* A class may have several constructors (0.6.5); without any, a class gets a no-argument constructor and a model gets the all-fields one.
  `return` is not allowed in a constructor. Constructors, methods, fields and properties can be `public` (default) or `hidden`.
* `public class` / `public model` make a class visible to the files that `use` its namespace; it can be named as a type there.
* Compound assignment (`+=` ...) works on variables and on fields (`x.count += 1`, `count += Step`).
* Inheritance and interfaces: see the next section. Not implemented: constructor overloading, shared properties, operators on objects, `==` structural comparison
  (`==` on two objects compares identity), object printing (`string(obj)` is an error).

## Inheritance and interfaces

```
interface Shape {                       # signatures only: no fields, no bodies
    method Area(): double
    method Name(): string
}
interface Solid : Shape { method Volume(): double }    # an interface can extend interfaces

class Animal {
    field string name
    protected field int legs = 4              # visible in Animal and its subclasses
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
* `hidden` members are visible only inside their own class; `protected` also in subclasses.
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
    field T value
    constructor(T v) { value = v }
    method Get(): T { return value }
}
class Pair<K, V> {
    field K key
    field V val
    constructor(K k, V v) { key = k; val = v }
    method Swap(): Pair<V, K> { return new Pair<V, K>(val, key) }
}
class Node<T> { T item; Node<T>? next }     # recursive generics work (a field of its own type must be nullable)
interface Source<T> { method Next(): T }    # generic interfaces too: class Counter : Source<int>

method First<T>(array<T> xs): T { return xs[0] }     # a generic top-level method
Box<int> b = new Box<int>(41)
Pair<string, int> p = new Pair<string, int>("a", 1)
Box<Box<int>> nested = new Box<Box<int>>(new Box<int>(5))
println("" + First<int>([7, 8]))                     # explicit type arguments always work
println("" + First([7, 8]))                          # since 0.6.1 they are usually inferred from the arguments
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

## Overloading (0.6.1)

Top-level methods, class methods (instance and static), interface methods and extension methods may share a name
when their parameter lists differ:

```
method Show(int x) : string { return "int " + x }
method Show(string s) : string { return "string " + s }
method Show(int a, int b) : string { ... }
println(Show(1) + Show("a") + Show(1, 2))
```

* The best candidate is chosen from the argument types: an exact match beats `null` to a nullable, which beats `int` to `double`,
  which beats an upcast, which beats boxing to `obj`/`dyn`; a variadic candidate loses to a fixed one. If two candidates are equally good you get
  `call to 'Show' is ambiguous: ...` (add a conversion such as `double(x)`); if none fits, `no overload of 'Show' accepts (...)` listing the candidates.
* Two declarations with the same parameter types are a duplicate, whatever the return types.
* A subclass method overrides the base method with the **same parameter types** (`override`); a different parameter list is a new overload of the subclass.
* Constructors cannot be overloaded (a class has one constructor). Hand declarations cannot be overloaded.
* Programs without overloads compile exactly as before; overloaded methods get internal names such as `Show$$int` (visible in ML and stack traces' ML names only).
* Studio's editor suggestions do not know about overloads yet (it lists a name once).

## Variadic parameters and extension methods

```
method sum(int... xs): int { ... }          # xs is an array<int> inside the method
sum()  sum(1)  sum(1, 2, 3)                 # extra arguments are packed into an array
sum(existing)                               # or pass one array<int> directly
method join(string sep, string... parts): string { ... }

method shout(here string s): string { return s + "!" }   # extension method
"hi".shout()                                # same as shout("hi")
method total(here array<int> a): int { return sum(a) }
```

* A variadic parameter must be the last one; at most one. `hand:` declarations cannot be variadic.
  Passing exactly one argument that already has the array type passes the array itself (no wrapping).
  Methods, class methods and constructors can be variadic.
* An extension method is a top-level method whose first parameter is marked `here`. `recv.name(...)`
  uses it only when the receiver has no built-in method (`push`, `copy`, map methods...) or class method
  of that name, so extensions cannot override those. It is also callable as a plain function. Extensions
  from another file need `public method` and a `use` of its namespace, like any method. Class members cannot be extensions,
  and extensions do not apply to `dyn` receivers (their calls are resolved at run time).
* Both are compile-time rewrites: the ML and the PVM are unchanged.

## Limits

Source files over 4 MiB (exit code 2), nesting deeper than 256 blocks/parentheses, and expressions
deeper than 1000 levels are rejected with a diagnostic.

### Type inference for generic methods (0.6.1)

`Id(5)`, `First(names)`, `Unbox(box)`, `Pick("a", "b", true)` need no `<...>`: the compiler matches the parameter types
(`T`, `array<T>`, `T?`, `map<K,V>`, `Box<T>`, `T...`) against the argument types. Arguments whose type it can work out
this way are: literals, declared and `var` locals, parameters, `new ...`, array literals, indexing, `.length`, arithmetic and
comparisons, conversions, fields, calls to methods that are not overloaded, and `keys()`/`values()`. If `T` is bound by two
different types (`Two(1, "s")`) or by nothing (`Make()` with `T` only in the return type) you get
`cannot infer type argument T of 'Make' ...; write it explicitly, e.g. Make<T>(...)`: write `Make<int>()`. `int` and `double` together widen to `double`.
A `null` argument says nothing about `T`. Inference works on top-level generic methods only (generic classes still need `new Box<int>(...)`).

## Statements

```
if (cond) { ... } else if (cond) { ... } else { ... }
if (cond) a = 1 else a = 2          # a body may be a single statement instead of a block (0.6.1)
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

Conditions must be `boolean` and need parentheses. Bodies of `if`/`else`/`while`/`for` are a braced block or one statement
(`while (i < 3) i++`).

In `for (init; cond; step)`, `init` is a declaration (`var i = 0`, `int i = 0`) or an assignment,
and `step` is an assignment or a call. A variable declared in `init` is visible only in the loop.
`x op= v` means `x = x op v`, with the same typing and runtime errors as the plain operator.
`break`/`continue` outside a loop are compile errors; they never cross a method boundary.

## Function values and lambdas (0.6.2)

```
fn(int) : int f            # a function type: parameter types, then ': result' (no result means void)
fn(int, int) : int add = (int a, int b) => a + b      # a lambda with an expression body
fn(int) : int inc = x => x + 1                         # parameter types may be left out when the context gives the type
fn(int) : string sign = (int n) : string => {          # a block body; ': type' after the parameters names the result
    if (n < 0) { return "neg" }
    return "pos"
}
fn() hello = () => println("hi")                       # no parameters, no result
println("" + add(1, 2) + " " + inc(4))                 # call a function value like a method
fn(int) : int d = Double                               # a top-level (non-overloaded) method used as a value
method Apply(fn(int) : int f, int x) : int { return f(x) }
Apply(n => n * 3, 5)                                   # lambda arguments take their types from the parameter
method Make(int k) : fn(int) : int { return x => x + k }
Make(3)(4)                                             # calling the result of a call
obj.callback(5)                                        # a field of function type is called the same way
```

* A lambda **shares the variables it captures** (0.8): a local variable that a lambda mentions and that is assigned anywhere (in the lambda
  or in the enclosing code) lives in a shared cell, so both sides always see the same value:
  `var n = 0; fn() inc = () => { n = n + 1 }; inc(); inc()` leaves `n` at 2, and a counter returned from a method keeps its own `n`.
  Parameters, loop variables and `dyn` variables are still captured **by value** (assigning to them inside a lambda is a compile error; copy them
  into a `var` first). Inside a class method a lambda can use `here`, fields and other methods.
* A lambda's own local variables must not reuse the name of a variable of the enclosing method (the same rule as for any redeclaration).
* Types of untyped parameters come from a declared variable type, a method parameter, a return type or an assignment. `var f = x => x` has no
  context, so it is an error: write `(int x) => x`. Result types of expression lambdas are inferred when no type is given.
* Function values cannot be `null`, are not nullable (`fn(int) : int?` returns an `int?`), cannot be boxed into `obj`/`dyn` and cannot be compared. A function-typed
  field or element that was never set holds a null reference; calling it is the runtime error `null reference: cannot call a null function value`.
* Generic inference does not look into lambdas: for `Map<T, R>(xs, f)` write `Map<int, string>(xs, f)`. Overloaded methods cannot be used as values.
* A lambda cannot be `async` or recursive through itself (use a method).
* Native backend: programs using function values or `try` are outside the native subset (see [llvm](llvm.md)) and run as bundled executables.

## Errors: exceptions, `try`, `catch`, `finally`, `throw` (0.8)

```
class NotFound : Exception {                      # your own exception types derive from Exception
    constructor(string what) : base("not found: " + what) { }
}
try {
    risky()
} catch (e: NotFound) {                            # typed catch: the first matching clause runs
    println("missing: " + e.Message)
} catch (e: RuntimeError) {                        # runtime failures (division by zero, index out of range, host errors ...)
    println("runtime: " + e.Message + "\n" + e.StackTrace)
} catch (e) {                                     # catch-all (an Exception); also: catch { ... }
    println("other: " + e)                        # "Error: message" through ToString()
} finally {
    println("always runs")
}
throw new NotFound("x")
throw "text"                                      # sugar for throw new Exception("text")
```

* **`Exception`** is a built-in class: `Message`, `StackTrace` (filled when the exception is caught), constructors `()` and `(string)`, and
  `ToString()` = `"Error: " + Message`. **`RuntimeError : Exception`** is what the runtime raises (arithmetic, bounds, null, failed host functions, ...).
  Both names are reserved: a program class called `Exception` or `RuntimeError` is an error. Only objects of Exception classes can be thrown.
* **Typed catch**: `catch (e: Type)` runs when the thrown object `is` that type (subclasses match). Clauses are tried in order; a catch after a
  catch-all is an error; when nothing matches the exception continues to the caller. `catch (string e)` and `catch (var e)` are errors.
* **`finally`** runs on every way out of the `try`/`catch` blocks: normal end, `return`, `break`, `continue` and a thrown exception.
  `return`, and `break`/`continue` that leave the `finally`, are not allowed inside it.
* An error nobody catches ends the program with `<file>:<line>: runtime error: <message>`, one `  at Method (file.coy:line)` line per active call
  (innermost first; long stacks are shortened to the first 20 and last 5 frames) and exit code 1. `throw e` rethrows keeping the original trace.
* `return`, `break` and `continue` work inside `try` and `catch`. Out-of-memory errors are not catchable.
* `try`, `catch`, `finally`, `throw` are keywords. The `sys.exception` mouth holds the text `Error: message`; `sys.exceptionObject` the thrown
  object and `sys.stackTrace` its trace.
* **Migrating from 0.7**: `catch (e)` now gives an `Exception` object, not a string. Use `e.Message` where the text is needed (`"" + e` prints
  `Error: ...`); `catch (string e)` becomes `catch (e)`. `python3 tools/migrate-0.8.py file.coy ...` does the usual cases.

## File-level variables (0.8)

Variables declared at the top level of the entry file are visible inside methods and classes of that file:

```
var counter = 0
method Bump() { counter = counter + 1 }
Bump(); Bump()                    # counter is 2
```

* Only a top-level `var`/typed declaration that some method of the file mentions becomes shared (a method that declares its own variable or parameter of that name, or whose class has a field or property of that name, does not count as mentioning it, and then cannot use the file-level one); a local of the same name inside a method shadows it,
  and class fields win over a file-level name. A method that runs before the declaration line sees the default value (0, null).
* They are initialized when the top-level code reaches the declaration. `(ref)` cannot alias them. Other files (namespaces) do not see them.

## The object protocol (0.8)

Well-known method names (PascalCase) that the language uses; no interface is needed, a matching method is enough:

| Method | Used by |
|---|---|
| `ToString(): string` | `"text" + obj`, `string(obj)`, `catch (e)` messages |
| `Equals(other): boolean` | `a == b` and `a != b` between two objects of that class (a comparison with `null` stays a reference check) |
| `Hash(): int`, `CompareTo(other): int` | reserved names for the 0.10 library (map keys, `List.Sort`); nothing calls them yet |

Without `Equals`, `==` on objects compares references.

## Qualified names (0.8)

A declaration of a namespace can be written with its namespace in front: `Acme.Geo.Point`, `new Acme.Geo.Point(1, 2)`, `Acme.Geo.Point.Origin()`,
`Acme.Geo.Add(1, 2)`. This works for everything the file can see (its own namespace and the namespaces it `use`s). When two used namespaces export
the same method name, the plain name is an error that asks for the qualified one. Dotted namespace names (`Acme.Geo`) are plain names: there is
no nesting or inheritance between `Acme` and `Acme.Geo`, no alias and no `use` of single names (decided for 1.0).

## `(copy)` and `(ref)` (0.6.5)

Assignment copies **values** for `int`, `double`, `boolean`, `char`, `string`, `Guid`, but **shares** objects, arrays and maps (`var b = a` makes
`b` another name for the same object). Two prefix forms change that:

```
var p = new Point(1, 2)
var q = (copy)p            # q is a NEW object with p's field values (shallow: nested objects/arrays/maps are shared)
q.X = 9                    # p.X is still 1
array<int> a = [1, 2, 3]
var b = (copy)a            # an independent array (same as a.copy())
var m2 = (copy)someMap     # an independent map

var n = 5
var r = (ref)n             # r IS n: both names share one storage cell, for any type (int, string, objects, ...)
r = r + 10                 # n is 15 now
n = 100                    # r reads 100
```

* `(copy)expr` works on class instances, arrays and maps (anything else is an error: plain values are already copied on assignment). The copy is
  shallow; it does not run constructors. Copying `null` is a runtime error; a nullable value must be unwrapped first.
* `(ref)name` is only allowed as the initializer of a declaration (`var r = (ref)n` or `int r = (ref)n`; the declared type must be the type of `n`).
  `r` and `n` then name the same variable, also when one of them is reassigned. The variable you alias must be declared with `var` or a type **in the same
  function**: parameters, `in (var x of ...)` variables, catch variables and variables captured by a lambda cannot be aliased (copy them into a local first).
  A lambda that mentions an aliased variable still captures the value it had when the closure was created, as before.
* How it works: a variable that some `(ref)` aliases is stored in a one-element array (a cell) and every use reads/writes element 0; the alias declaration
  shares the cell. No new instruction is needed for `(ref)`. `(copy)` of an array uses `acopy`; of an object or a map it uses the new `ocopy` (ML 9).
* `copy` and `ref` are not reserved words: `(copy)` / `(ref)` are recognized only when the closing parenthesis is followed by an operand.
* Native backend: programs that use objects or maps run as bundled PVM executables (see [llvm](llvm.md)); `(ref)` on `int`/`string` variables in the native subset
  works the same way because it compiles to arrays, which are outside the subset, so those programs are bundled too.

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

## `mouth:<module> <name> {get;}` (0.6.5.2)

`hand` brings in host **functions**; `mouth` brings in host **variables**. A mouth is a name whose value the host (the PVM) provides each time it is read:

```
mouth:sys exception {get;}            # the message of the last exception a `catch` caught (null before the first)
mouth:sys os                          # no accessor block = read-only, same as {get;}
mouth:sys exitCode {get; set;}        # the host allows writing this one

try { throw "boom" } catch (e) { }
println(string(exception))            # boom
exitCode = 3                          # the program ends with exit code 3
```

* Every mouth is an `obj` (it can hold an `int`, a `string`, a `boolean`, ... or null): use `is`, `string(m)`, `int(m)` ... to get the value out, see "Obj and dyn".
* **Read-only by default.** Assigning needs `{get; set;}` in the declaration, and the host must really allow writing (the verifier refuses a program that declares `set` for a read-only host mouth).
  `{set;}` alone is write-only. Anything else inside the braces is an error.
* **Copyable by default.** Every read produces a fresh value, so `var saved = exception` is independent of later changes; `(copy)m` of an obj is allowed and returns the value itself (boxed values are immutable).
* Like `hand`, a mouth is declared at the top level and is visible in its own file (in methods and classes too; a local variable of the same name hides it). Declaring grants nothing: reading or writing
  needs the mouth's permission at the access, and a program that declares mouths it never uses does not carry them.
* Built-in mouths (all in the `sys` module): `exception` (get), `exitCode` (get, set; applies when the program ends normally; `sys.exit` still wins), `os` (`windows`/`linux`/`macos`), `version` (the Coyote version), `pid` (needs `system`).
* ML 10: `mouth <module>.<name> [get] [set]` declarations and `getmouth` / `setmouth` instructions ([spec](ml-spec.md)). Native backend: programs that use mouths are outside the native subset for now (bundled).

## Changes in 0.7 that can break old programs

* `enum`, `switch`, `case` and `default` are keywords now (rename variables with those names).
* `Time.UptimeMs()`, `Time.UnixSeconds()`, `File.Size()` and `File.ModifiedUnix()` return **`long`**; `Time.Format/FormatUtc` take a `long`. Where you stored them in an `int`, write `int(x)` or switch the variable to `long`.
* The `Timer` library's `ElapsedMs()` and `DueIn()` return `long`.

## Reserved but not implemented

Using any of these gives `'<word>' is not supported yet` (and they cannot be used as names):

`created operator module`

`import` was removed in 0.6 (use `namespace` + `use`); `namespace` is a keyword now.

Also not implemented: negative literals
in `hand` calls (use an expression), top-level globals (use `shared` class fields), shared properties, map literals,
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

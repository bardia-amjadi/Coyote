# Coyote 0.8 — formal grammar (language freeze candidate)

This is the grammar of the language as the 0.8 parser accepts it. From the freeze on (see [compatibility](compatibility.md)) the constructs
below only change in backward-compatible ways until a new major generation. The parser (`src/compiler/parser.cpp`) is the final authority; if
the two ever disagree, that is a bug in one of them: report it.

Notation: `A B` sequence, `A | B` choice, `[A]` optional, `{A}` zero or more, `'x'` a literal token. A newline or `;` ends a statement
(a `[` or `.` after an expression must be on the same line as the expression).
Comments start with `#` and run to the end of the line.

## Files

```
file        = [ 'namespace' qname ] { use } { declaration | statement }
use         = 'use' STRING
qname       = IDENT { '.' IDENT }
declaration = hand | mouth | method | class | model | interface | enum
```
Only the entry file may contain statements. `namespace` must be first. `public` before a declaration exports it.

## Declarations

```
hand       = 'hand' ':' IDENT type? ... (host function declaration, see ml-spec)
mouth      = 'mouth' ...                        (host variable declaration, see ml-spec)
method     = [ 'public' ] 'method' IDENT [ '<' IDENT {',' IDENT} '>' ] '(' [params] ')' [ ':' type ] block
class      = [ 'public' ] 'class' IDENT [ '<' IDENT {',' IDENT} '>' ] [ ':' type {',' type} ] '{' { member } '}'
member     = field | property | constructor | method-in-class
field      = ['private'|'protected'|'public'] ['shared'] ['readonly'|'locked'] 'field' type IDENT ['=' expr]
constructor= [access] 'constructor' '(' [params] ')' [ ':' 'base' '(' args ')' ] block
enum       = [ 'public' ] 'enum' IDENT '{' IDENT ['=' expr] { ',' IDENT ['=' expr] } '}'
params     = param { ',' param }        param = type IDENT | type '...' IDENT
```

## Types

```
type       = base [ '?' ]
base       = 'int' | 'long' | 'byte' | 'double' | 'boolean' | 'char' | 'string' | 'Guid' | 'obj' | 'dyn'
           | 'array' '<' type '>' | 'map' '<' key '>' ',' type '>'
           | 'fn' '(' [type {',' type}] ')' [ ':' type ]
           | qname [ '<' type {',' type} '>' ]            (class, model, interface, enum; qualified allowed)
```

## Statements

```
statement  = vardecl | assignment | call | if | while | for | foreach | switch | try | throw | return
           | 'break' | 'continue' | block
vardecl    = ( 'var' | type ) IDENT [ '=' expr ]
assignment = lvalue ( '=' | '+=' | '-=' | '*=' | '/=' | '%=' | '&=' | '|=' | '^=' | '<<=' | '>>=' ) expr | lvalue ( '++' | '--' )
if         = 'if' '(' expr ')' stmt-or-block [ 'else' stmt-or-block ]
while      = 'while' '(' expr ')' block
for        = 'for' '(' [vardecl|assignment] ';' [expr] ';' [assignment|call] ')' block
foreach    = 'for' '(' ( 'var' | type ) IDENT 'in' expr ')' block          (also: 'in (var c of s)' over a string)
try        = 'try' block { 'catch' '(' IDENT [ ':' qname ] ')' block | 'catch' block } [ 'finally' block ]
throw      = 'throw' expr
```

## Expressions (lowest to highest precedence)

```
expr       = coalesce
coalesce   = or { '??' or }
or         = and { '||' and }          and = bitor { '&&' bitor }
bitor      = bitxor { '|' bitxor }     bitxor = bitand { '^' bitand }     bitand = equality { '&' equality }
equality   = relation { ('==' | '!=') relation }
relation   = shift { ('<' | '<=' | '>' | '>=' | 'is' type) shift }
shift      = additive { ('<<' | '>>') additive }
additive   = term { ('+' | '-') term }          term = unary { ('*' | '/' | '%') unary }
unary      = ('-' | '!' | '~' | '(copy)' | '(ref)') unary | postfix
postfix    = primary { '.' IDENT | '.' IDENT '(' args ')' | '[' expr ']' | '(' args ')' | '!' }
primary    = literal | IDENT | 'here' | 'new' qname ['<'types'>'] '(' args ')' | 'new' 'array' '<' type '>' '(' expr ')'
           | 'new' 'map' '<' key ',' type '>' '(' ')' | '[' [expr {',' expr}] ']' | '(' expr ')' | lambda | switch-expr
lambda     = ( IDENT | '(' [lparam {',' lparam}] ')' ) [ ':' type ] '=>' ( expr | block )
switch-expr= 'switch' '(' expr ')' '{' pattern {',' pattern} '=>' expr { ',' ... } '}'
literal    = INT | LONG ('5L') | HEX ('0xFF') | BINARY ('0b101') | DOUBLE | STRING | CHAR | 'true' | 'false' | 'null'
```

## Reserved words (final for 1.0)

`hand mouth method var int long byte string boolean if else while for break continue return true false array new double char Guid obj dyn map
null in is use namespace enum switch case default class model interface property field constructor public private protected override static
this try catch finally throw hidden shared here locked readonly`, plus the names of the built-in classes `Exception` and `RuntimeError`.
(`static`, `this`, `import`, `export` are kept only to give a "was renamed" message.)

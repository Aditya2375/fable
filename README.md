# fable

fable is a small, complete programming language: a tree-walk-free, bytecode
interpreter written in C11 plus the POSIX.1-2008 getline and open_memstream functions (the Makefile builds with -std=gnu11), with no dependencies beyond libc and libm. It has
lexically scoped variables, first-class functions, closures with shared
upvalues, lists, strings, and a mark-sweep garbage collector. The interpreter core is
3019 lines of C.

Try it in your browser: **https://aditya2375.github.io/fable/** - the same C
compiled to WebAssembly, running entirely on your machine.

## Quick start

```sh
make            # builds build/fable and build/test-unit
make test       # unit suite + golden cases + docs check
./build/fable                  # REPL (multi-line input continues with a "... " prompt)
./build/fable script.fable     # run a file
./build/fable -c 'print(1+2)'  # run a snippet
./build/fable -disasm x.fable  # dump the compiled bytecode, then run
```

```fable
fn fib(n) {
    if (n < 2) { return n; }
    return fib(n - 1) + fib(n - 2);
}

for (let i = 0; i < 10; i = i + 1) {
    print(fib(i));
}
```

## The language

### Values

| Type      | Literals                              |
|-----------|---------------------------------------|
| nil       | `nil`                                 |
| bool      | `true`, `false`                       |
| number    | `42`, `3.5`, `1e3` (IEEE 754 double)  |
| string    | `"hello"` (immutable, interned)       |
| list      | `[1, "two", false]` (growable, mixed) |
| function  | `fn(x) { return x; }`                 |

Only `nil` and `false` are falsy. `0` and `""` are truthy.

### Variables and scope

`let` declares; assignment without `let` updates an existing binding. Blocks
(`{ ... }`), function bodies, and `for` headers create lexical scopes.
Reading or assigning an undeclared variable is a runtime error (globals are
resolved when the program runs, not when it is compiled).

### Functions and closures

Functions are first-class and capture their surrounding variables by
reference. Captures are shared: two closures over the same local see each
other's writes.

```fable
fn make_counter() {
    let n = 0;
    fn step() { n = n + 1; return n; }
    return step;
}
let c = make_counter();
print(c());   # 1
print(c());   # 2
```

Recursion works through the `let`/`fn name` binding. Anonymous functions
(`fn (x) { ... }`) can be passed and stored like any value.

### Control flow

`if` / `else`, `while`, C-style `for (init; cond; step)`, and `return`.
`and` / `or` short-circuit and yield one of their operands (not a coerced
bool); `not` yields a bool.

### Lists and strings

```fable
let xs = [1, 2, 3];
push(xs, 4);        # [1, 2, 3, 4]
print(xs[-1]);      # 4 (negative indices count from the end)
print(pop(xs));     # 4
xs[0] = 99;
print(len(xs));

let s = "hello";
print(s[0]);        # "h" (strings index by byte; they are immutable)
print(len(s));      # 5
```

### Operators

Arithmetic `+ - * / %`, comparison `< <= > >=`, equality `== !=`, unary
`-` and `not`. `+` adds numbers, concatenates strings, and concatenates lists without mutating either operand (`[1] + [2]` is `[1, 2]`). Division by zero
yields IEEE `inf` (matching `1.0/0.0` in C); `%` by zero is a runtime error.
`==` compares numbers by value, strings by content, everything else by
identity.

### Built-ins

`print(...)` (variadic: arguments print space-separated),
`len(string|list)`, `push(list, v)`, `pop(list)`, `str(v)`,
`num(string|bool)`, `type(v)`, `clock()`, `abs(x)`, `floor(x)`, `sqrt(x)`.

### Comments and syntax

`#` and `//` start line comments; `/* ... */` is a block comment (block
comments nest and must be closed). Statements end with `;`. In the REPL and
the browser demo, a line that leaves a bracket, string or comment open
continues on the next line with a `... ` prompt.

`str(v)` renders any value exactly the way `print` would (lists, cycles
included; `str(f)` is `<fn name>`).

`num()` is strict: on a string it accepts exactly the source-literal
grammar (`-12.5e3` - no `+` sign, digits on both sides of the point, no
length cap, matching the lexer) plus the exact
words `inf`/`nan` (so `num(str(x))` round-trips - except `nan`, which by
IEEE rule never equals itself), and rejects whitespace, hex, empty strings
and trailing garbage. On a bool it returns 1 or 0; a number passes through
unchanged.

In the terminal REPL, ending input with a bracket, string or comment still
open is an error (`unexpected end of input`, nonzero exit), and so is a
NUL byte anywhere in the input (`NUL byte in source`, nonzero exit) - input
lines are length-aware, capped only with the accumulated input at 64 KB.
In the browser
demo, clicking an example while a line is half-typed discards the partial
line (and says so) instead of wedging the session.

## Errors

Compile errors report the line and a caret-free excerpt; runtime errors print
a message plus the line. Running a script file or `-c`, any error exits nonzero
and cleans up the VM; a script file containing a NUL byte is rejected up front
(`NUL byte in source file`, naming its line) rather than silently truncated.
In the REPL a compile or runtime error prints and the session continues, so the
exit status stays 0; only input that ends with a bracket, string or comment
still open, a NUL byte in the input, or accumulated multi-line input
over 64 KB exits nonzero. REPL errors name the absolute input line, even when
the failing text sits inside a multi-line statement; the browser demo numbers
lines the same way.
Statements before the failing one in the same input still commit: globals
they defined stay defined, their prints already happened; the failing
statement commits nothing (a `let` that fails leaves the name undefined) -
there is no statement-level atomicity. The browser demo's wasm boundary
rejects NUL bytes the same way the CLI does: a loud error naming the absolute
input line, with any half-typed multi-line input dropped (the page marshals
an explicit length, so input is never silently truncated). `fable script.fable -c "..."` is a usage error
(exit 2): give a script or `-c`, not both.

```
$ fable -c 'let x = ;'
<cmdline>:1: at ';': expected an expression, got ';'
```

Every error names its source and line: `file.fable:12: message`.

## Honest limits

fable is deliberately small. It does **not** have:

- `break` / `continue` in loops (restructure with flags or functions)
- maps / dictionaries (lists and functions cover the demos)
- string comparison operators beyond `==` / `!=` (no `<` on strings)
- modules, imports, classes, or exceptions
- a standard library beyond the built-ins above
- tail-call optimization

Resource limits, each enforced with a clean error (never a crash) and each
pinned by the unit, CLI or fuzz suites: call depth is capped at 1024 frames
(65536 stack values, `stack overflow` past that, traces capped at 12 frames
plus a `... N more frames` summary); expressions nest at most 512 levels -
each nested construct (parentheses, list brackets, unary operators) costs
exactly one level, mixed or not, and so does each function-call or
list-element layer (an argument or element nests 511 constructs deep
inside its brackets, 510 inside two layers, and so on) - blocks 256,
statement chains 256,
function definitions 256; a function takes at most 255 parameters and
scopes at most 255 locals (a frame has 256 slots; slot 0 is the function
itself), a closure captures at most 256 upvalues, and a call passes at
most 255 arguments; a list literal is built element by element, so its
size is bounded by memory, not the stack, and has no element-count cap; one
chunk holds at most 65536 constants; a jump spans at most 64 KB of bytecode
(`jump too far` forward, `loop body too large` backward); string
concatenation caps the result at 256 MB
(`string too long` past that); list printing detects
cycles (`[[...]]`) and caps nesting at 64 levels (`...`). In the REPL,
accumulated multi-line input is capped at 64 KB (`input too long`). After any depth
guard fires, parsing stops with that one error - every parse loop exits
instead of spinning, and no further diagnostics (including lexer errors in
the remaining input) are emitted (regression-tested with a 10-second
wall-clock guard in the shell and fuzz suites, natively and in the wasm
smoke test).

Named function *expressions* (`let f = fn g() {...}`) bind only `f`; the inner
name is display metadata, not a self-reference. Use a `fn` declaration or a
`let` binding for recursion.

Numbers are doubles, so integers above 2^53 lose precision, as in JavaScript.
Integral values with magnitude at most 2^53 print as plain integers
(`1694577218885960`, not `1.69457721888596e+15`); every other value prints
in the shortest form that round-trips: `num(str(x))` is
bit-identical to `x` for every number except `nan` (IEEE: `nan != nan`),
`-0` keeps its sign, and `nan`/`inf`/`-inf` print as words.

## How it works

```
source → lexer → single-pass Pratt compiler → bytecode → stack VM
                                            ↑ mark-sweep GC over all objects
```

- **Lexer** (`lexer.c`): one token of lookahead, hand-written.
- **Compiler** (`compiler.c`): Pratt parser emitting bytecode in a single
  pass - no AST is ever built. Local variables live on the VM stack;
  closures capture locals through upvalues resolved at compile time and
  closed when their scope exits.
- **Bytecode** (`chunk.c`): a compact instruction set (constants deduped
  through a hash table, jumps,
  calls, closures, list/string indexing) with per-instruction line info.
- **VM** (`vm.c`): a stack machine with a call-frame stack. Globals live in
  an open-addressed hash table.
- **Memory** (`gc.c`, `obj.c`): mark-sweep collection rooted at the value
  stack, call frames, globals, open upvalues, and the compiler's in-flight
  function chain. Strings are interned, so `==` on equal strings is a
  pointer compare. Collection triggers when the live heap passes a
threshold (1 MiB at first, then twice the live size after each pass; the
byte counter tracks live bytes exactly, so long sessions don't ratchet); `make stress` recompiles with `-DFABLE_GC_STRESS` to
  collect on every allocation.

## Testing

- `make test` runs everything:
  - a C unit suite (`tests/unit/test-unit.c`) with **884 checks** covering
    the lexer, values, hash table, bytecode chunks, and end-to-end programs
    (closures, recursion, shared upvalues, lists, error paths, IEEE edge
    cases, GC-safety patterns, and every resource limit above);
  - a golden suite (`tests/cases/`, 11 golden programs) diffing real
    interpreter output against committed expectations;
  - `tests/check-docs.sh`, which fails if this README's test counts or line
    count drift.
- `make sanitize` re-runs the whole suite under AddressSanitizer,
  UndefinedBehaviorSanitizer, and float-cast-overflow checking.
- `make stress` re-runs it with the GC collecting on every allocation.
- `python3 tests/fuzz.py` generates random integer-arithmetic programs and
  diffs fable's output against python3 (200 programs and 100 byte blobs by
  default; the blobs are random bytes that must never crash the
  interpreter). CI runs 100 programs + 60 byte blobs across nine seeds,
  plus 50 programs + 300 byte blobs under the sanitizers.

The CI workflow runs the unit, golden, sanitizer, GC-stress, and fuzz suites
plus a WebAssembly build on every push.

## Layout

```
src/        interpreter core (lexer, compiler, vm, gc, objects, table, chunk, value)
wasm/       Emscripten wrapper exposing fable_boot / fable_exec_len
docs/       browser demo (GitHub Pages), built from wasm/
tests/      C unit suite, golden cases, docs check, wasm smoke test
```

The design follows the proven tree-walk-free architecture for small
languages (single-pass compilation to a stack VM); all code here is written
for this project.

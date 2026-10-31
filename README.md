# fable

fable is a small, complete programming language: a tree-walk-free, bytecode
interpreter written in C11 with no dependencies beyond libc and libm. It has
lexically scoped variables, first-class functions, closures with shared
upvalues, lists, strings, and a mark-sweep garbage collector. The interpreter core is
2687 lines of C.

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
`-` and `not`. `+` adds numbers and concatenates strings. Division by zero
yields IEEE `inf` (matching `1.0/0.0` in C); `%` by zero is a runtime error.
`==` compares numbers by value, strings by content, everything else by
identity.

### Built-ins

`print(x)`, `len(string|list)`, `push(list, v)`, `pop(list)`, `str(v)`,
`num(string)`, `type(v)`, `clock()`, `abs(x)`, `floor(x)`, `sqrt(x)`.

### Comments and syntax

`#` and `//` start line comments; `/* ... */` is a block comment (block
comments nest and must be closed). Statements end with `;`. In the REPL and
the browser demo, a line that leaves a bracket, string or comment open
continues on the next line with a `... ` prompt.

`str(list)` renders a list exactly the way `print` would, cycles included.

## Errors

Compile errors report the line and a caret-free excerpt; runtime errors print
a message plus the line. Both exit with a nonzero status and clean up the VM.

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

Resource limits, all enforced with clean errors rather than crashes: call
depth is capped at 1024 frames (65536 stack values); expressions nest at
most 512 levels, blocks 256, function definitions 128; list printing detects
cycles (`[[...]]`) and caps nesting at 64 levels (`...`).

Named function *expressions* (`let f = fn g() {...}`) bind only `f`; the inner
name is display metadata, not a self-reference. Use a `fn` declaration or a
`let` binding for recursion.

Numbers are doubles, so integers above 2^53 lose precision, as in JavaScript.
Printing uses the shortest form that round-trips: `num(str(x)) == x` always
holds, `-0` keeps its sign, and `nan`/`inf`/`-inf` print as words.

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
- **Bytecode** (`chunk.c`): a compact instruction set (constants, jumps,
  calls, closures, list/string indexing) with per-instruction line info.
- **VM** (`vm.c`): a stack machine with a call-frame stack. Globals live in
  an open-addressed hash table.
- **Memory** (`gc.c`, `obj.c`): mark-sweep collection rooted at the value
  stack, call frames, globals, open upvalues, and the compiler's in-flight
  function chain. Strings are interned, so `==` on equal strings is a
  pointer compare. Collection triggers at a heap threshold (1 MiB, doubling
  after each pass); `make stress` recompiles with `-DFABLE_GC_STRESS` to
  collect on every allocation.

## Testing

- `make test` runs everything:
  - a C unit suite (`tests/unit/test-unit.c`) with **518 checks** covering
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
  diffs fable's output against python3 (300 programs), plus 100 random byte
  blobs that must never crash the interpreter (also run under the sanitizers
  in CI).

The CI workflow runs the unit, golden, sanitizer, GC-stress, and fuzz suites
plus a WebAssembly build on every push.

## Layout

```
src/        interpreter core (lexer, compiler, vm, gc, objects, table, chunk, value)
wasm/       Emscripten wrapper exposing fable_boot / fable_exec
docs/       browser demo (GitHub Pages), built from wasm/
tests/      C unit suite, golden cases, docs check, wasm smoke test
```

The design follows the proven tree-walk-free architecture for small
languages (single-pass compilation to a stack VM); all code here is written
for this project.

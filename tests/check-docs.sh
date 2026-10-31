#!/bin/sh
# Keeps README claims honest: the unit-check count and golden-case count in
# README.md must match what the suites actually contain.
set -u
cd "$(dirname "$0")/.."
fail=0
if [ ! -x ./build/test-unit ]; then echo "check-docs: build first (make)" >&2; exit 1; fi
actual=$(./build/test-unit | grep -o '[0-9]* checks passed' | head -1 | cut -d' ' -f1)
if ! grep -q "$actual checks" README.md; then
    echo "check-docs: README unit-check count stale (suite: $actual checks passed)"; fail=1
fi
cases=$(ls tests/cases/*.fable | wc -l)
if ! grep -q "$cases golden" README.md; then
    echo "check-docs: README golden-case count ($cases) missing or stale"; fail=1
fi
prog=$(sed -n 's/.*"--programs", type=int, default=\([0-9][0-9]*\).*/\1/p' tests/fuzz.py)
blob=$(sed -n 's/.*"--blobs", type=int, default=\([0-9][0-9]*\).*/\1/p' tests/fuzz.py)
if ! grep -q "$prog programs and $blob byte blobs" README.md; then
    echo "check-docs: README fuzz counts stale (fuzz.py defaults: $prog programs, $blob byte blobs)"; fail=1
fi
lines=$(cat src/*.c src/*.h | wc -l)
if ! grep -q "$lines lines of C" README.md; then
    echo "check-docs: README line count ($lines) missing or stale"; fail=1
fi
# Pin the documented limits (README "Honest limits") to the code that
# enforces them: each pair is <file with the enforcing check>|<pattern there>|<README phrase>.
pins="
src/compiler.c|at most 255 parameters|at most 255 parameters
src/compiler.c|at most 255 arguments|255 arguments
src/compiler.c|too many local variables in one function|at most 255 locals
src/compiler.c|too many captured variables in one function|at most 256 upvalues
src/compiler.c|list literal too long|65535 elements
src/compiler.c|too many constants in one chunk|65536 constants
src/compiler.c|jump too far|jump too far
src/compiler.c|loop body too large|loop body too large
src/main.c|input too long|input too long
src/main.c|NUL byte in source file|NUL byte
src/obj.h|FABLE_STRING_MAX|256 MB
"
printf '%s' "$pins" | while IFS='|' read -r file src_pat doc_pat; do
    [ -n "$file" ] || continue
    if ! grep -qF "$src_pat" "$file"; then
        echo "check-docs: enforcing code missing: $src_pat in $file"; exit 1
    fi
    if ! grep -qF "$doc_pat" README.md; then
        echo "check-docs: README does not document: $doc_pat"; exit 1
    fi
done || fail=1
[ "$fail" -eq 0 ] && echo "check-docs: ok"
exit $fail

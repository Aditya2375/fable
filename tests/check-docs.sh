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
[ "$fail" -eq 0 ] && echo "check-docs: ok"
exit $fail

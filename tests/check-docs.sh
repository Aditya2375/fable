#!/bin/sh
# Keeps README claims honest: the unit-check count and golden-case count in
# README.md must match what the suites actually contain.
set -u
cd "$(dirname "$0")/.."
fail=0
if [ ! -x ./build/test-unit ]; then echo "check-docs: build first (make)" >&2; exit 1; fi
actual=$(./build/test-unit | sed -n 's/.*\([0-9][0-9]*\) checks passed.*/\1/p' | head -1)
if ! grep -q "$actual checks" README.md; then
    echo "check-docs: README unit-check count stale (suite: $actual checks passed)"; fail=1
fi
cases=$(ls tests/cases/*.fable | wc -l)
if ! grep -q "$cases golden" README.md; then
    echo "check-docs: README golden-case count ($cases) missing or stale"; fail=1
fi
[ "$fail" -eq 0 ] && echo "check-docs: ok"
exit $fail

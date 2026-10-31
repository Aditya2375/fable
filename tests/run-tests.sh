#!/bin/sh
# Golden-case suite: each tests/cases/NAME.fable must print exactly NAME.out.
set -u
cd "$(dirname "$0")/.."
BIN=./build/fable
if [ ! -x "$BIN" ]; then echo "run-tests: build first (make)" >&2; exit 1; fi
fail=0
count=0
for f in tests/cases/*.fable; do
    name="${f%.fable}"
    count=$((count + 1))
    if ! "$BIN" "$f" > /tmp/fable-got.out 2>/tmp/fable-got.err; then
        echo "FAIL $name (exit $?):"; cat /tmp/fable-got.err; fail=1; continue
    fi
    if ! diff -u "$name.out" /tmp/fable-got.out > /tmp/fable-diff.out; then
        echo "FAIL $name:"; cat /tmp/fable-diff.out; fail=1
    fi
done
if [ "$fail" -eq 0 ]; then echo "golden: $count cases passed"; fi
exit $fail

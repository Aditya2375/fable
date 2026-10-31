#!/bin/sh
# Keeps README claims honest: unit-check count and golden-case count must match
# what the test suites actually contain.
set -u
cd "$(dirname "$0")/.."
fail=0
unit=$(grep -c 'OK(\|CHECK(' tests/unit/test-unit.c | head -1)
if ! grep -q "471 checks" README.md 2>/dev/null; then
    echo "check-docs: README unit-check count missing or stale"; fail=1
fi
cases=$(ls tests/cases/*.fable | wc -l)
if ! grep -q "$cases golden" README.md; then
    echo "check-docs: README golden-case count ($cases) missing or stale"; fail=1
fi
[ "$fail" -eq 0 ] && echo "check-docs: ok"
exit $fail

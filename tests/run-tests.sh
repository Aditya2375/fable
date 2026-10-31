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
# CLI behavior checks that golden stdout files cannot express
cli_fail=0
out=$(printf 'fn f() {' | "$BIN" 2>&1) && rc=$? || rc=$?
if [ "$rc" -eq 0 ] || ! printf '%s' "$out" | grep -q "unexpected end of input"; then
    echo "FAIL cli: REPL EOF with incomplete input must error and exit nonzero (rc=$rc: $out)"; cli_fail=1
fi
out=$("$BIN" -c 'print(1+1);' 2>&1) && rc=$? || rc=$?
if [ "$rc" -ne 0 ] || [ "$out" != "2" ]; then
    echo "FAIL cli: -c print(1+1) (rc=$rc: $out)"; cli_fail=1
fi
# EOF with a comment hiding an open bracket: incomplete input, not a program
out=$(printf '{\n# c' | "$BIN" 2>&1) && rc=$? || rc=$?
if [ "$rc" -eq 0 ] || ! printf '%s' "$out" | grep -q "unexpected end of input"; then
    echo "FAIL cli: REPL EOF inside comment with open brace (rc=$rc: $out)"; cli_fail=1
fi
# an over-long input line is an error with a nonzero exit, not a silent skip
out=$(awk 'BEGIN{for(i=0;i<5000;i++)printf "1"}' | "$BIN" 2>&1) && rc=$? || rc=$?
if [ "$rc" -eq 0 ] || ! printf '%s' "$out" | grep -q "line too long"; then
    echo "FAIL cli: over-long line must error and exit nonzero (rc=$rc: $out)"; cli_fail=1
fi
[ "$cli_fail" -eq 0 ] && echo "cli: 4 checks passed" || fail=1

if [ "$fail" -eq 0 ]; then echo "golden: $count cases passed"; fi
exit $fail

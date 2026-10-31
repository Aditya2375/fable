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

# Depth-guard termination (round-5 HIGH1): a guard firing mid-parse must end
# in one clean error, in every position - pre-fix these spun at 100% CPU
# forever (and froze the wasm demo tab). Wall-clock guarded: a regression
# fails here by timeout, not by hanging CI.
term_fail=0
term_count=0
deep=$(awk 'BEGIN{for(i=0;i<513;i++)printf "("; printf "7"; for(i=0;i<513;i++)printf ")"}')
chain="fn f() { $(awk 'BEGIN{for(i=0;i<260;i++)printf "if (1) "}')x; print(1); }"
blocks="fn f() { $(awk 'BEGIN{for(i=0;i<300;i++)printf "{"; for(i=0;i<300;i++)printf "}"}') print(1); }"
fns=$(awk 'BEGIN{for(i=0;i<300;i++)printf "fn g%d() { ", i; printf "x;"; for(i=0;i<300;i++)printf "}"}')
term_check() {
    term_count=$((term_count + 1))
    out=$(timeout 10 "$BIN" -c "$1" 2>&1) && rc=$? || rc=$?
    if [ "$rc" -eq 124 ]; then
        echo "FAIL terminate: hung (10s): $2"; term_fail=1; return
    fi
    if [ "$rc" -eq 0 ]; then
        echo "FAIL terminate: expected a compile error: $2"; term_fail=1; return
    fi
    if ! printf '%s' "$out" | grep -q 'too deeply'; then
        echo "FAIL terminate: no depth-guard error: $2 ($out)"; term_fail=1
    fi
}
term_check "fn f() { let x = $deep; y; }" "guard in fn body, middle statement"
term_check "fn f() { y; let x = $deep; }" "guard in fn body, last statement"
term_check "fn f() { { let x = $deep; } print(1); }" "guard in nested block"
term_check "{ let x = $deep; } print(1);" "guard in top-level block"
term_check "fn g(a) { return a; } fn f() { let x = g($deep); y; }" "guard in call args"
term_check "fn f() { let xs = [$deep]; y; }" "guard in list literal"
term_check "fn f() { let y = 1; return $deep; }" "guard in return value"
term_check "fn f() { while ($deep) { } y; }" "guard in while condition"
term_check "fn f() { for (let i = $deep; i < 3; i = i + 1) { } y; }" "guard in for clause"
term_check "$chain" "statement-chain guard with following statement"
term_check "$blocks" "block-nesting guard with following statement"
term_check "$fns" "fn-nesting guard"
[ "$term_fail" -eq 0 ] && echo "terminate: $term_count checks passed" || fail=1

if [ "$fail" -eq 0 ]; then echo "golden: $count cases passed"; fi
exit $fail

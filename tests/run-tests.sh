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
# round-7 F2: input lines have no length cap (getline) - a 9000-char line
# just runs
out=$(awk 'BEGIN{printf "print(1"; for(i=0;i<3000;i++)printf "+1"; printf ");\n"}' | "$BIN" 2>&1) && rc=$? || rc=$?
if [ "$rc" -ne 0 ] || [ "$out" != "3001" ]; then
    echo "FAIL cli: a 9000-char input line must run (rc=$rc: $out)"; cli_fail=1
fi
# round-7 F2: a NUL byte on stdin is an error with its line and a nonzero
# exit, never a silent truncation - mid-line, final-line and unterminated
out=$(printf 'print(1\000);\nprint("after");\n' | "$BIN" 2>/tmp/fable-repl-nul.err) && rc=$? || rc=$?
if [ "$rc" -eq 0 ] || ! grep -q '<repl>:1: NUL byte' /tmp/fable-repl-nul.err || [ "$out" != "after" ]; then
    echo "FAIL cli: NUL mid-line must drop that line, error nonzero, continue (rc=$rc: $out)"; cli_fail=1
fi
out=$(printf 'print(1);\n\000print(999);' | "$BIN" 2>/tmp/fable-repl-nul2.err) && rc=$? || rc=$?
if [ "$rc" -eq 0 ] || ! grep -q '<repl>:2: NUL byte' /tmp/fable-repl-nul2.err || [ "$out" != "1" ]; then
    echo "FAIL cli: NUL final-line must error nonzero, earlier lines still ran (rc=$rc: $out)"; cli_fail=1
fi
out=$(printf 'print(7)\000' | "$BIN" 2>/tmp/fable-repl-nul3.err) && rc=$? || rc=$?
if [ "$rc" -eq 0 ] || ! grep -q '<repl>:1: NUL byte' /tmp/fable-repl-nul3.err || [ -n "$out" ]; then
    echo "FAIL cli: unterminated NUL line must error nonzero (rc=$rc: $out)"; cli_fail=1
fi
# round-6 F2: a NUL byte in a script file is an error with its line,
# never a silently truncated program
{ printf 'print(1);\nprint(2);'; printf '\000'; printf 'print(3);\n'; } > /tmp/fable-nul.fable
out=$("$BIN" /tmp/fable-nul.fable 2>/tmp/fable-nul.err) && rc=$? || rc=$?
if [ "$rc" -eq 0 ] || ! grep -q 'fable: /tmp/fable-nul.fable:2: NUL byte' /tmp/fable-nul.err; then
    echo "FAIL cli: NUL byte must error nonzero with its line (rc=$rc: $(cat /tmp/fable-nul.err))"; cli_fail=1
fi
if [ -n "$out" ]; then
    echo "FAIL cli: NUL script executed a prefix (stdout: $out)"; cli_fail=1
fi
# round-6 F4: a script and -c together are a usage error, not a silent choice
out=$("$BIN" /tmp/fable-nul.fable -c 'print(222);' 2>&1) && rc=$? || rc=$?
if [ "$rc" -ne 2 ] || ! printf '%s' "$out" | grep -q 'not both'; then
    echo "FAIL cli: script + -c must be a usage error with exit 2 (rc=$rc: $out)"; cli_fail=1
fi
# round-6 F3: in the REPL, compile and runtime errors print and the session
# continues - exit stays 0
out=$(printf 'let x = ;\nprint("after");\n' | "$BIN" 2>/dev/null) && rc=$? || rc=$?
if [ "$rc" -ne 0 ] || ! printf '%s' "$out" | grep -q after; then
    echo "FAIL cli: REPL must survive a compile error with exit 0 (rc=$rc: $out)"; cli_fail=1
fi
out=$(printf 'print(nope);\nprint("after");\n' | "$BIN" 2>/dev/null) && rc=$? || rc=$?
if [ "$rc" -ne 0 ] || ! printf '%s' "$out" | grep -q after; then
    echo "FAIL cli: REPL must survive a runtime error with exit 0 (rc=$rc: $out)"; cli_fail=1
fi
# round-8 F3: REPL errors name the absolute input line, even when the
# failing text sits inside a multi-line statement
out=$(printf 'print(111);\nlet x = [\nnope];\n' | "$BIN" 2>&1) && rc=$? || rc=$?
if ! printf '%s' "$out" | grep -q '<repl>:3: undefined variable'; then
    echo "FAIL cli: REPL error must name the absolute input line 3 (rc=$rc: $out)"; cli_fail=1
fi
# REPL multi-line input over 64 KB: "input too long", nonzero exit
out=$(awk 'BEGIN{for(i=0;i<17;i++){for(j=0;j<4000;j++)printf "("; printf "\n"}printf ")\n"}' | "$BIN" 2>&1) && rc=$? || rc=$?
if [ "$rc" -eq 0 ] || ! printf '%s' "$out" | grep -q "input too long"; then
    echo "FAIL cli: over-64KB pending input must error and exit nonzero (rc=$rc: $out)"; cli_fail=1
fi
# the same 64 KB cap applies to one long line (there is no shorter per-line cap)
out=$(awk 'BEGIN{printf "print(1"; for(i=0;i<33000;i++)printf "+1"; printf ");\n"}' | "$BIN" 2>&1) && rc=$? || rc=$?
if [ "$rc" -eq 0 ] || ! printf '%s' "$out" | grep -q "input too long"; then
    echo "FAIL cli: over-64KB single line must error and exit nonzero (rc=$rc: $out)"; cli_fail=1
fi
# round-11 F2: script reading goes to EOF, never trusts st_size
out=$("$BIN" /proc/version 2>&1) && rc=$? || rc=$?
if [ "$rc" -eq 0 ]; then
    echo "FAIL cli: /proc/version (st_size 0, real content) must not run as an empty program"; cli_fail=1
fi
fifo=/tmp/fable-fifo.$$
mkfifo "$fifo"
printf 'print(7);\n' > "$fifo" &
out=$("$BIN" "$fifo" 2>&1) && rc=$? || rc=$?
wait
rm -f "$fifo"
if [ "$rc" -ne 0 ] || [ "$out" != "7" ]; then
    echo "FAIL cli: a FIFO script must be read to EOF and run (rc=$rc: $out)"; cli_fail=1
fi
out=$("$BIN" /tmp 2>&1) && rc=$? || rc=$?
if [ "$rc" -eq 0 ] || ! printf '%s' "$out" | grep -q "cannot read"; then
    echo "FAIL cli: a directory must be a clean read error (rc=$rc: $out)"; cli_fail=1
fi
: > /tmp/fable-empty.fable
out=$("$BIN" /tmp/fable-empty.fable 2>&1) && rc=$? || rc=$?
rm -f /tmp/fable-empty.fable
if [ "$rc" -ne 0 ]; then
    echo "FAIL cli: an empty script is a valid empty program (rc=$rc: $out)"; cli_fail=1
fi
[ "$cli_fail" -eq 0 ] && echo "cli: 19 checks passed" || fail=1

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
# round-8 F2: constant dedupe is hash-based - the critic's 65536-distinct-
# constant compile took ~2.9s pre-fix, milliseconds post-fix; 2s is the guard
term_count=$((term_count + 1))
awk 'BEGIN{for(i=0;i<=65535;i++)printf "%d.1;\n", i}' > /tmp/fable-manyconst.fable
out=$(timeout 2 "$BIN" /tmp/fable-manyconst.fable 2>&1) && rc=$? || rc=$?
if [ "$rc" -ne 0 ]; then
    echo "FAIL terminate: 65536 distinct constants must compile well under 2s (rc=$rc)"; term_fail=1
fi
[ "$term_fail" -eq 0 ] && echo "terminate: $term_count checks passed" || fail=1

if [ "$fail" -eq 0 ]; then echo "golden: $count cases passed"; fi
exit $fail

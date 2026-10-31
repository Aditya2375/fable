#!/usr/bin/env python3
"""Two fuzz harnesses for fable:

1. Differential: generate random programs in the integer-arithmetic subset
   that maps 1:1 onto Python, run each under both fable and python3, and
   diff the outputs byte for byte.
2. Byte fuzz: throw random byte strings at the interpreter; it must always
   exit normally (any exit code), never crash (signal) and never trigger a
   sanitizer report.

Usage: python3 tests/fuzz.py [--bin PATH] [--programs N] [--blobs N] [--seed S]
"""
import argparse
import random
import subprocess
import sys
import tempfile
import os


def gen_leaf(rng, vars_):
    if vars_ and rng.random() < 0.6:
        v = rng.choice(vars_)
        return v, v
    n = str(rng.randint(-20, 20))
    return n, n


def gen_expr(rng, vars_, depth, allow_mul=True):
    """An integer-valued expression as a (fable, python) pair, with
    magnitudes bounded far below 2^53 (fable numbers are doubles): `*` only
    ever multiplies a leaf by a small constant, and never appears where a
    loop body could compound it. `%` appears only as leaf % small nonzero
    constant; fable % is C fmod, so the oracle uses math.fmod, which agrees
    with it on every integer input (round-9 L5: the generator really emits
    the % this docstring used to only talk about)."""
    if depth == 0 or (vars_ and rng.random() < 0.35):
        return gen_leaf(rng, vars_)
    if rng.random() < 0.15:
        f, p = gen_leaf(rng, vars_)
        k = rng.randint(2, 97)
        return f"({f} % {k})", f"int(math.fmod({p}, {k}))"
    if allow_mul and rng.random() < 0.25:
        f, p = gen_leaf(rng, vars_)
        k = rng.randint(-12, 12)
        return f"({f} * {k})", f"({p} * {k})"
    op = rng.choice(["+", "-"])
    f1, p1 = gen_expr(rng, vars_, depth - 1, allow_mul)
    f2, p2 = gen_expr(rng, vars_, depth - 1, allow_mul)
    return f"({f1} {op} {f2})", f"({p1} {op} {p2})"


def gen_cond(rng, vars_):
    af, ap = gen_expr(rng, vars_, 2)
    bf, bp = gen_expr(rng, vars_, 2)
    op = rng.choice(["<", "<=", ">", ">=", "==", "!="])
    return f"{af} {op} {bf}", f"{ap} {op} {bp}"


def gen_program(rng):
    """Parallel fable/python sources. Integer arithmetic only; fable % is C
    fmod, so the python oracle translates it with math.fmod (they agree on
    all integer inputs, signs included). Some programs carry 65-120 distinct
    string constants in one chunk, exercising the constant-table build at
    the 64-constant boundary with fresh strings (round-9 HIGH)."""
    flines, plines = [], []
    vars_ = []
    n = rng.randint(3, 8)
    nstrings = 0

    def emit(f, p, indent=0):
        flines.append("    " * indent + f)
        plines.append("    " * indent + p)

    for _ in range(n):
        kind = rng.random()
        if kind < 0.12:
            # >64 distinct string constants: the dedupe table is built/grown
            # while these fresh strings are only reachable via the round-9
            # compile-time shield
            sv = f"str{nstrings}"
            nstrings += 1
            lits = ['"lit%d_%d"' % (j, rng.randrange(10**9)) for j in range(rng.randint(65, 120))]
            cat = " + ".join(lits)
            emit(f"let {sv} = {cat}; print(len({sv}));",
                 f"{sv} = {cat}\nprint(len({sv}))")
        elif kind < 0.4 or not vars_:
            v = f"v{len(vars_)}"
            vars_.append(v)
            ef, ep = gen_expr(rng, vars_[:-1], 3)
            emit(f"let {v} = {ef};", f"{v} = {ep}")
        elif kind < 0.6:
            v = rng.choice(vars_)
            ef, ep = gen_expr(rng, vars_, 3)
            emit(f"{v} = {ef};", f"{v} = {ep}")
        elif kind < 0.8:
            cf, cp = gen_cond(rng, vars_)
            e1f, e1p = gen_expr(rng, vars_, 2)
            e2f, e2p = gen_expr(rng, vars_, 2)
            emit(f"if ({cf}) {{ print({e1f}); }} else {{ print({e2f}); }}",
                 f"print({e1p}) if ({cp}) else print({e2p})")
        else:
            # while loops must provably terminate: guard with a counter
            v = rng.choice(vars_)
            guard = f"g{len(vars_)}"
            vars_.append(guard)
            ef, ep = gen_expr(rng, vars_[:-1], 2, allow_mul=False)
            cf, cp = gen_cond(rng, vars_[:-1])
            emit(f"let {guard} = 0; while ({cf} and {guard} < 25) {{ {v} = {ef}; {guard} = {guard} + 1; }}",
                 f"{guard} = 0\nwhile ({cp}) and ({guard} < 25):\n        {v} = {ep}\n        {guard} = {guard} + 1")
        if rng.random() < 0.5:
            ef, ep = gen_expr(rng, vars_, 2)
            emit(f"print({ef});", f"print({ep})")
    return "\n".join(flines) + "\n", "import math\n" + "\n".join(plines) + "\n"


def run(cmd, timeout=10):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout, p.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="build/fable")
    ap.add_argument("--programs", type=int, default=200)
    ap.add_argument("--blobs", type=int, default=100)
    ap.add_argument("--seed", type=int, default=20261101)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    fails = 0
    skipped_big = 0
    skipped_neg0 = 0

    with tempfile.TemporaryDirectory() as td:
        ff = os.path.join(td, "p.fable")
        pf = os.path.join(td, "p.py")
        for i in range(args.programs):
            fable_src, py_src = gen_program(rng)
            open(ff, "w").write(fable_src)
            open(pf, "w").write(py_src)
            frc, fout, ferr = run([args.bin, ff])
            prc, pout, perr = run([sys.executable, pf])
            if frc != 0:
                print(f"FAIL program {i}: fable exit {frc}\n{ferr}\n---\n{fable_src}")
                fails += 1
                continue
            if prc != 0:
                print(f"note program {i}: generator emitted bad python (skipped)\n{perr}")
                continue
            # Compare line by line so one over-range line cannot mask a real
            # bug on the other lines (round-9 L5): the oracle is exact
            # integers, fable numbers are doubles, so only the lines where
            # python reaches 2^53 legitimately diverge and are skipped.
            fl = fout.split("\n")
            pl = pout.split("\n")
            if len(fl) != len(pl):
                print(f"FAIL program {i}: line count differs\nfable:\n{fout}\npython:\n{pout}\n---\n{fable_src}")
                fails += 1
                continue
            for a, b in zip(fl, pl):
                if b and b.lstrip("-").isdigit() and abs(int(b)) >= 2**53:
                    skipped_big += 1
                    continue
                # fable prints IEEE -0 where the true result is 0 (0 * -3,
                # fmod of a -0); python ints have no -0 at all, so that exact
                # pair is accepted - but a -0 against any other oracle line
                # is a real bug (round-9 L5: no blanket -0 normalization).
                if a == "-0" and b == "0":
                    skipped_neg0 += 1
                    continue
                if a != b:
                    print(f"FAIL program {i}: fable {a!r} vs python {b!r}\nfable:\n{fout}\npython:\n{pout}\n---\n{fable_src}")
                    fails += 1
                    break

        for i in range(args.blobs):
            blob = bytes(rng.randrange(0, 256) for _ in range(rng.randrange(1, 400)))
            # keep it mostly printable-ish half the time to hit deeper parser paths
            if i % 2 == 0:
                blob = bytes(rng.choice(b"abcxyz0123 (){}[];+-*/%<>=!\"#letfnif") for _ in range(len(blob)))
            open(ff, "wb").write(blob)
            p = subprocess.run([args.bin, ff], capture_output=True, timeout=10)
            if p.returncode < 0 or p.returncode > 100:
                print(f"FAIL blob {i}: crashed/sanitized with exit {p.returncode}\n{p.stderr.decode(errors='replace')[:400]}\nblob: {blob!r}")
                fails += 1
            elif b"Sanitizer" in p.stderr or b"runtime error:" in p.stderr.lower() and b"UBSan" in p.stderr:
                print(f"FAIL blob {i}: sanitizer report\n{p.stderr.decode(errors='replace')[:400]}\nblob: {blob!r}")
                fails += 1

    # Termination tripwires (round-5 HIGH1): a depth guard firing in any
    # position - fn body, nested block, call args, list literal, unbraced
    # chain, deep blocks, deep fns - must end in one clean error. Pre-fix
    # the fn-body parse loop spun forever; a regression times out here.
    deep = "(" * 513 + "7" + ")" * 513
    chain = "fn f() { " + "if (1) " * 260 + "x; print(1); }"
    blocks = "fn f() { " + "{" * 300 + "}" * 300 + " print(1); }"
    fns = "".join(f"fn g{i}() {{ " for i in range(300)) + "x;" + "}" * 300
    tripwires = [
        f"fn f() {{ let x = {deep}; y; }}",
        f"fn f() {{ y; let x = {deep}; }}",
        f"fn f() {{ {{ let x = {deep}; }} print(1); }}",
        f"{{ let x = {deep}; }} print(1);",
        f"fn g(a) {{ return a; }} fn f() {{ let x = g({deep}); y; }}",
        f"fn f() {{ let xs = [{deep}]; y; }}",
        f"fn f() {{ let y = 1; return {deep}; }}",
        f"fn f() {{ while ({deep}) {{ }} y; }}",
        f"fn f() {{ for (let i = {deep}; i < 3; i = i + 1) {{ }} y; }}",
        chain, blocks, fns,
    ]
    for i, src in enumerate(tripwires):
        try:
            p = subprocess.run([args.bin, "-c", src], capture_output=True, text=True, timeout=10)
        except subprocess.TimeoutExpired:
            print(f"FAIL tripwire {i}: did not terminate in 10s")
            fails += 1
            continue
        if p.returncode == 0 or "too deeply" not in p.stderr:
            print(f"FAIL tripwire {i}: exit {p.returncode}, stderr: {p.stderr[:200]}")
            fails += 1
    # NUL-byte tripwires (round-6 F2): a script file containing NUL must be
    # rejected with a clear error - never silently truncated so a prefix runs.
    nul_cases = [
        b"print(1);\x00print(2);",
        b"\x00",
        b"let x = 1;\n\x00\nprint(x);",
        b"print(1);\nprint(2);\n\x00",
    ]
    nul_ok = 0
    with tempfile.TemporaryDirectory() as td2:
        nf = os.path.join(td2, "nul.fable")
        for i, blob in enumerate(nul_cases):
            open(nf, "wb").write(blob)
            p = subprocess.run([args.bin, nf], capture_output=True, timeout=10)
            if p.returncode == 0 or p.stdout or b"NUL byte" not in p.stderr:
                print(f"FAIL nul {i}: rc {p.returncode} stdout {p.stdout[:80]!r} stderr {p.stderr[:120]!r}")
                fails += 1
            else:
                nul_ok += 1
    # NUL-byte tripwires for the REPL (round-7 F2): piped stdin with a NUL
    # must be rejected loudly (nonzero exit, "NUL byte" on stderr), never
    # silently truncated so a prefix or suffix runs unnoticed.
    repl_nul_cases = [
        (b"print(1\x00);\n", b""),                    # mid-line: nothing runs
        (b"print(1);\x00print(999);", b""),            # one tainted line: nothing runs
        (b"print(7)\x00", b""),                        # unterminated at EOF
        (b"\x00", b""),                                # only a NUL
        (b"let x = 1;\nprint(x\x00);\nprint(2);\n", b"2\n"),  # bad line dropped, session continues
    ]
    repl_nul_ok = 0
    for i, (blob, want) in enumerate(repl_nul_cases):
        p = subprocess.run([args.bin], input=blob, capture_output=True, timeout=10)
        if p.returncode == 0 or b"NUL byte" not in p.stderr or p.stdout != want:
            print(f"FAIL repl-nul {i}: rc {p.returncode} stdout {p.stdout[:80]!r} stderr {p.stderr[:120]!r}")
            fails += 1
        else:
            repl_nul_ok += 1
    print(f"fuzz: {len(tripwires)} termination tripwires + {nul_ok} file NUL + {repl_nul_ok} REPL NUL tripwires passed" if not fails else "")

    if fails:
        print(f"fuzz: {fails} failures")
        return 1
    big = f", {skipped_big} lines past the 2^53 oracle (skipped)" if skipped_big else ""
    big += f", {skipped_neg0} IEEE -0 lines accepted" if skipped_neg0 else ""
    print(f"fuzz: {args.programs} differential programs + {args.blobs} byte blobs passed (seed {args.seed}{big})")
    return 0


if __name__ == "__main__":
    sys.exit(main())

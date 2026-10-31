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
        return rng.choice(vars_)
    return str(rng.randint(-20, 20))


def gen_expr(rng, vars_, depth, allow_mul=True):
    """An integer-valued expression valid in both fable and python3, with
    magnitudes bounded far below 2^53 (fable numbers are doubles): `*` only
    ever multiplies a leaf by a small constant, and never appears where a
    loop body could compound it."""
    if depth == 0 or (vars_ and rng.random() < 0.35):
        return gen_leaf(rng, vars_)
    if allow_mul and rng.random() < 0.25:
        return f"({gen_leaf(rng, vars_)} * {rng.randint(-12, 12)})"
    op = rng.choice(["+", "-"])
    return f"({gen_expr(rng, vars_, depth - 1, allow_mul)} {op} {gen_expr(rng, vars_, depth - 1, allow_mul)})"


def gen_cond(rng, vars_):
    a = gen_expr(rng, vars_, 2)
    b = gen_expr(rng, vars_, 2)
    op = rng.choice(["<", "<=", ">", ">=", "==", "!="])
    return f"{a} {op} {b}"


def gen_program(rng):
    """Parallel fable/python sources. Integer-only, % only as mod-1000 idiom
    (fable % is fmod; on non-negative integers it matches Python's %)."""
    flines, plines = [], []
    vars_ = []
    n = rng.randint(3, 8)

    def emit(f, p, indent=0):
        flines.append("    " * indent + f)
        plines.append("    " * indent + p)

    for _ in range(n):
        kind = rng.random()
        if kind < 0.4 or not vars_:
            v = f"v{len(vars_)}"
            vars_.append(v)
            emit(f"let {v} = {gen_expr(rng, vars_[:-1], 3)};",
                 f"{v} = {gen_expr(rng, vars_[:-1], 3)}")
            # re-emit identical expr is wasteful; regenerate identically instead
            flines[-1] = "let %s = %s;" % (v, plines[-1].split(" = ", 1)[1])
        elif kind < 0.6:
            v = rng.choice(vars_)
            e = gen_expr(rng, vars_, 3)
            emit(f"{v} = {e};", f"{v} = {e}")
        elif kind < 0.8:
            c = gen_cond(rng, vars_)
            e1 = gen_expr(rng, vars_, 2)
            e2 = gen_expr(rng, vars_, 2)
            emit(f"if ({c}) {{ print({e1}); }} else {{ print({e2}); }}",
                 f"print({e1}) if ({c}) else print({e2})")
        else:
            # while loops must provably terminate: guard with a counter
            v = rng.choice(vars_)
            guard = f"g{len(vars_)}"
            vars_.append(guard)
            e = gen_expr(rng, vars_[:-1], 2, allow_mul=False)
            c = gen_cond(rng, vars_[:-1])
            emit(f"let {guard} = 0; while ({c} and {guard} < 25) {{ {v} = {e}; {guard} = {guard} + 1; }}",
                 f"{guard} = 0\nwhile ({c}) and ({guard} < 25):\n        {v} = {e}\n        {guard} = {guard} + 1")
        if rng.random() < 0.5:
            e = gen_expr(rng, vars_, 2)
            emit(f"print({e});", f"print({e})")
    return "\n".join(flines) + "\n", "\n".join(plines) + "\n"


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
            # IEEE vs Python ints: fable prints -0 for 0 * -3, Python has no -0.
            norm = lambda t: "\n".join("0" if l == "-0" else l for l in t.split("\n"))
            if norm(fout) != norm(pout):
                print(f"FAIL program {i}: output differs\nfable:\n{fout}\npython:\n{pout}\n---\n{fable_src}")
                fails += 1

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

    if fails:
        print(f"fuzz: {fails} failures")
        return 1
    print(f"fuzz: {args.programs} differential programs + {args.blobs} byte blobs passed (seed {args.seed})")
    return 0


if __name__ == "__main__":
    sys.exit(main())

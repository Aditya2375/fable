/* Smoke test for the browser (wasm) build: boots the demo module and checks
   the session API - REPL echo, print output, persistent state across calls,
   error recovery, and reset. Run after sh wasm/build.sh:
   node tests/wasm-smoke.js [path/to/fable.js] */
const factory = require(process.argv[2] || require("path").join(__dirname, "..", "docs", "fable.js"));
let out = "";
let err = "";
let fails = 0;
function ok(cond, label) {
    if (cond) console.log("ok: " + label);
    else { console.log("FAIL: " + label); fails = 1; }
}
factory({ print: s => { out += s + "\n"; }, printErr: s => { err += s + "\n"; } }).then(m => {
    const boot = () => m.ccall("fable_boot", "number", [], []);
    const run = (src) => {
        out = ""; err = "";
        const rc = m.ccall("fable_exec", "number", ["string"], [src]);
        return { rc, out, err };
    };
    ok(boot() === 0, "wasm: boot");

    let r = run("print(6 * 7);");
    ok(r.rc === 0 && r.out.includes("42"), "wasm: print works");

    r = run("1 + 2;");
    ok(r.out.includes("3"), "wasm: expression echoes in session mode");

    /* L1 regression: a trailing-newline EOF error reports the content line,
     * matching the terminal REPL */
    r = run("1 + 1");
    ok(r.rc === 1 && r.err.includes("<web>:1: at end"), "wasm: EOF error reports the content line");

    run("fn square(x) { return x * x; }");
    r = run("print(square(9));");
    ok(r.out.includes("81"), "wasm: definitions persist across calls");

    run("let c = 0;");
    run("fn bump() { c = c + 1; return c; }");
    run("bump(); bump();");
    r = run("print(bump());");
    ok(r.out === "3\n", "wasm: closure over shared global (exact output, no echo leak)");

    /* HIGH2 regression: the demo examples must print EXACTLY this - no
     * echo leaking out of function or loop bodies. Example buttons run
     * with echo off, like the page does. */
    m.ccall("fable_set_echo", null, ["number"], [0]);
    r = run("fn make_counter() {\n  let n = 0;\n  fn step() { n = n + 1; return n; }\n  return step;\n}\nlet mc = make_counter();\nprint(mc());\nprint(mc());\nlet md = make_counter();\nprint(md());\nprint(mc());");
    ok(r.out === "1\n2\n1\n3\n", "wasm: closures example prints exactly 1 2 1 3");
    r = run("let xs = [1, 2, 3];\npush(xs, 4);\nprint(xs);\nprint(xs[-1]);\nprint(pop(xs));\nlet squares = [];\nfor (let i = 0; i < 6; i = i + 1) {\n  push(squares, i * i);\n}\nprint(squares);");
    ok(r.out === "[1, 2, 3, 4]\n4\n4\n[0, 1, 4, 9, 16, 25]\n", "wasm: lists example exact output");
    r = run("for (let i = 1; i <= 15; i = i + 1) {\n  if (i % 15 == 0) { print(\"fizzbuzz\"); }\n  else if (i % 3 == 0) { print(\"fizz\"); }\n  else if (i % 5 == 0) { print(\"buzz\"); }\n  else { print(i); }\n}");
    ok(r.out === "1\n2\nfizz\n4\nbuzz\nfizz\n7\n8\nfizz\nbuzz\n11\nfizz\n13\n14\nfizzbuzz\n", "wasm: fizzbuzz example exact output");
    m.ccall("fable_set_echo", null, ["number"], [1]);

    /* MED2 regression: an example button must unwedge a pending line */
    let wedge = run("fn half() {");
    ok(wedge.rc === 2, "wasm: half-typed fn is pending");
    ok(m.ccall("fable_discard_pending", "number", [], []) === 1, "wasm: discard reports pending input");
    ok(m.ccall("fable_discard_pending", "number", [], []) === 0, "wasm: discard is quiet when nothing pending");
    r = run("print(6 * 7);");
    ok(r.rc === 0 && r.out === "42\n", "wasm: session works right after a discard");

    /* MED9: multi-line input accumulates across exec calls */
    let rc2 = run("fn seven() {");
    ok(rc2.rc === 2 && rc2.out.includes("..."), "wasm: incomplete input asks for more lines");
    r = run("  return 7;\n}");
    ok(r.rc === 0, "wasm: completed function runs");
    r = run("print(seven());");
    ok(r.out === "7\n", "wasm: multi-line definition works");

    /* HIGH3/MED5 in the browser build */
    r = run("let cyc = []; push(cyc, cyc); print(cyc);");
    ok(r.out === "[[...]]\n[[...]]\n", "wasm: cyclic list prints [...] (push echo + print)");
    r = run("print(1e300); print(-0.0); print(0/0 <= 1);");
    ok(r.out === "1e+300\n-0\nfalse\n", "wasm: 1e300 / -0 / NaN <= exact");

    r = run("let xs = []; for (let i = 0; i < 5; i = i + 1) { push(xs, i * i); } print(xs);");
    ok(r.rc === 0 && r.out.includes("[0, 1, 4, 9, 16]"), "wasm: lists and loops");

    r = run("let x = ;");
    ok(r.rc !== 0 && r.err.includes("<web>:1:"), "wasm: compile error names the source and line");
    r = run("print(1 + 1);");
    ok(r.rc === 0 && r.out.includes("2"), "wasm: session survives a compile error");

    /* R5 HIGH1 regression: a depth-guard error inside a function body must
     * return promptly - pre-fix fable_exec never returned and froze the tab */
    r = run("fn f() { let x = " + "(".repeat(513) + "7" + ")".repeat(513) + "; y; }");
    ok(r.rc === 1 && r.err.includes("expression nested too deeply"), "wasm: depth guard in fn body returns");
    r = run("print(2 + 2);");
    ok(r.rc === 0 && r.out.includes("4"), "wasm: session survives a depth-guard error");

    /* R6 F1 regression: after a depth guard, trailing lexer errors must not
     * cascade - exactly one error line */
    r = run("let x = " + "(".repeat(513) + "7" + ")".repeat(513) + "; @");
    ok(r.rc === 1 && r.err.includes("expression nested too deeply") && !r.err.includes("unexpected character"), "wasm: no lexer-error cascade after a depth guard");

    r = run("print(undefined_variable);");
    ok(r.rc !== 0, "wasm: runtime error reported");
    r = run("print(1 + 1);");
    ok(r.rc === 0 && r.out.includes("2"), "wasm: session survives a runtime error");

    /* GC pressure across many calls in one session */
    let leaked = false;
    for (let i = 0; i < 300; i++) {
        run('let s' + i + ' = "x" + "y";');
        r = run('let t = ["' + "a".repeat(50) + '", 1, 2, 3]; print(len(t));');
        if (!r.out.includes("4")) leaked = true;
    }
    ok(!leaked, "wasm: 600 allocations under GC stay correct");

    ok(boot() === 0, "wasm: reset boots a fresh VM");
    r = run("print(square(9));");
    ok(r.rc !== 0, "wasm: reset drops earlier definitions");
    r = run("print(2 + 2);");
    ok(r.rc === 0 && r.out.includes("4"), "wasm: fresh VM works after reset");

    /* round-10 F2: the demo page must pass the input line to fable_exec
     * untrimmed (a trailing space inside a string continued across lines is
     * source), keeping only the empty-line check. Extract the real handler
     * and runLine from docs/index.html and drive them with stubs. */
    const html = require("fs").readFileSync(require("path").join(__dirname, "..", "docs", "index.html"), "utf8");
    const rl = html.match(/function runLine\(line\)\{[^}]*\}/)[0];
    const key = html.match(/if \(e\.key === 'Enter'[^}]*\}/)[0];
    let sent = null;
    const printCmd = () => {};
    const exec = src => { sent = src; };
    const inp = { value: 'let s = "abc ' };   // trailing space inside an open string
    const e = { key: "Enter" };
    eval(rl + ";" + key);
    ok(sent === 'let s = "abc ', "demo: Enter sends the line untrimmed");
    inp.value = "   ";
    sent = null;
    eval(key);
    ok(sent === null, "demo: whitespace-only line stays a no-op");

    process.exit(fails);
});

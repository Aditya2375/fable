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
    /* Marshal with an explicit length: ccall's "string" conversion would
     * silently truncate at a NUL (round-13 F1). Mirrors the demo page. */
    const run = (src) => {
        out = ""; err = "";
        const bytes = new TextEncoder().encode(src);
        const ptr = m._malloc(Math.max(bytes.length, 1));
        m.writeArrayToMemory(bytes, ptr);
        const rc = m.ccall("fable_exec_len", "number", ["number", "number"], [ptr, bytes.length]);
        m._free(ptr);
        return { rc, out, err };
    };
    ok(boot() === 0, "wasm: boot");

    let r = run("print(6 * 7);");
    ok(r.rc === 0 && r.out.includes("42"), "wasm: print works");

    r = run("1 + 2;");
    ok(r.out.includes("3"), "wasm: expression echoes in session mode");

    /* L1 regression: a trailing-newline EOF error reports the content line,
     * matching the terminal REPL. Fresh session: error lines are absolute
     * (round-16 F1), so boot to make the expected number deterministic. */
    boot();
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

    boot();
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

    /* round-12 HIGH-1: a runtime error closes open upvalues - a closure
     * that escaped to a global keeps its captured value afterwards, and the
     * session continues correctly */
    run("let g = nil;");
    run("fn outer() { let x = 42; fn getter() { return x; } g = getter; nil + 1; }");
    r = run("outer();");
    ok(r.rc !== 0, "wasm: runtime error propagates");
    r = run("print(g());");
    ok(r.out.includes("42"), "wasm: escaped closure keeps its local after an error unwind");

    /* round-13 F1: a NUL byte at the wasm boundary is a loud error, never
     * a silent truncation - all three critic repros */
    r = run("print(1);\0 @@@ garbage");
    ok(r.rc !== 0 && r.out === "" && r.err.includes("NUL byte"),
       "wasm: NUL after valid code rejects the whole input");
    r = run("\0let lost = 1;");
    ok(r.rc !== 0 && r.err.includes("NUL byte"), "wasm: leading NUL rejected");
    r = run("print(lost);");
    ok(r.rc !== 0 && r.err.includes("undefined variable"),
       "wasm: nothing from a NUL-tainted line gets defined");
    boot();
    r = run("fn nulholder() {");
    ok(r.rc === 2, "wasm: open fn leaves input pending");
    r = run("  return 1;");
    ok(r.rc === 2, "wasm: still pending after a body line");
    r = run("\0");
    ok(r.rc !== 0 && r.err.includes("<web>:3") && r.err.includes("NUL byte"),
       "wasm: NUL mid-pending names the absolute session line");
    r = run("}");
    ok(r.rc !== 0, "wasm: the NUL line drops the pending input, like the CLI");
    r = run("print(nulholder);");
    ok(r.rc !== 0 && r.err.includes("undefined variable"),
       "wasm: nothing from the dropped pending input survives");

    /* round-16 F1: compile errors name the absolute session line, exactly
     * like the terminal REPL (the critic repro: CLI <repl>:3) */
    boot();
    run("print(111);");
    run("let y = [");
    r = run("nope];");
    ok(r.rc === 1 && r.err.includes("<web>:3:"),
       "wasm: compile error inside a pending statement names the absolute session line");

    /* round-16 F3: accumulated input over 64 KB is rejected like the CLI,
     * dropping the pending statement */
    boot();
    r = run("fn capped() {");
    ok(r.rc === 2, "wasm: open fn pending for the cap test");
    r = run("  let pad = \"" + "x".repeat(70000) + "\";");
    ok(r.rc === 1 && r.err.includes("input too long"),
       "wasm: over-64KB accumulated input rejected like the CLI");
    r = run("}");
    ok(r.rc !== 0, "wasm: the oversized line drops the pending input, like the CLI");
    r = run("print(1 + 1);");
    ok(r.rc === 0 && r.out.includes("2"), "wasm: session survives an over-64KB rejection");

    /* round-10 F2: the demo page must pass the input line to fable_exec
     * untrimmed (a trailing space inside a string continued across lines is
     * source), keeping only the empty-line check. Extract the real handler
     * and runLine from docs/index.html and drive them with stubs. */
    const html = require("fs").readFileSync(require("path").join(__dirname, "..", "docs", "index.html"), "utf8");
    /* Extract a snippet or fail loudly - a silent [0] on null hides a rename */
    const grab = (re, what) => {
        const hit = html.match(re);
        if (!hit) console.log("FAIL: wasm-smoke could not find " + what + " in docs/index.html"), fails = 1;
        return hit && hit[0];
    };
    const rl = grab(/function runLine\(line\)\{[^}]*\}/, "runLine()");
    const key = grab(/if \(e\.key === 'Enter'[^}]*\}/, "the Enter key handler");
    let sent = null;
    const printCmd = () => {};
    const exec = src => { sent = src; };
    const inp = { value: 'let s = "abc ' };   // trailing space inside an open string
    const e = { key: "Enter" };
    if (rl && key) {
        eval(rl + ";" + key);
        ok(sent === 'let s = "abc ', "demo: Enter sends the line untrimmed");
        inp.value = "   ";
        sent = null;
        eval(key);
        ok(sent === null, "demo: whitespace-only line stays a no-op");
    }

    /* round-14: buttons start disabled until boot enables them, and the
     * js/wasm cache-busters match each other and the wasm content hash */
    const buttons = html.match(/<button data-p="[^"]+" disabled>/g) || [];
    ok(buttons.length === 6, "demo: all six buttons start disabled");
    ok(html.includes("b.disabled = false"), "demo: boot enables the buttons");
    const vjs = (html.match(/fable\.js\?v=([a-z0-9]{12})/) || [])[1];
    const vw = (html.match(/locateFile:[^;]*\?v=([a-z0-9]{12})/) || [])[1];
    const crypto = require("crypto");
    const wasmHash = crypto.createHash("sha256")
        .update(require("fs").readFileSync(require("path").join(__dirname, "..", "docs", "fable.wasm")))
        .digest("hex").slice(0, 12);
    ok(vjs && vjs === vw && vjs === wasmHash, "demo: cache-busters match the wasm hash");

    /* round-16 F2/F4: a rejecting FableModule lands in bootFailed, which
     * writes the reason into the output; the controls are only ever enabled
     * in the success path, so a failure leaves them disabled */
    ok(html.includes(".catch(bootFailed)"), "demo: boot rejection is wired to bootFailed");
    const bootSrc = grab(/function bootFailed\(e\)\{[\s\S]*?\n\}/, "bootFailed()");
    if (bootSrc) {
        const errs = [];
        const printErr = t => { errs.push(t); };
        const out = { textContent: "loading wasm..." };
        eval("var bootFailedPage = " + bootSrc);
        bootFailedPage(new Error("fetch 404"));
        ok(out.textContent === "" && errs.length === 2 && errs[0].includes("could not start") && errs[0].includes("fetch 404"),
           "demo: a failed boot writes the reason into the output");
    }

    /* round-15 F1 / round-16 F1: the page-side NUL belt names the same
     * absolute session line the C side computes and drops the pending input
     * like the CLI. Extract the real exec() and drive its NUL path with
     * stubs: session lines 1-2 are the pending statement, this is line 3. */
    const execSrc = grab(/function exec\(src\)\{[\s\S]*?\n\}/, "exec()");
    if (execSrc) {
        let perr = null, discarded = 0;
        const printErr = t => { perr = t; };
        let sessionLines = 2, pendingStart = 1, pendingLines = 2;
        const M = { ccall: name => { if (name === 'fable_discard_pending') discarded++; return 0; } };
        eval("var execPage = " + execSrc);   // expression: no clash with the round-10 exec stub
        execPage("let x = \0;");
        ok(perr && perr.includes("<web>:3") && discarded === 1 && pendingLines === 0 && sessionLines === 3,
           "demo: page-side NUL error matches the C-side absolute line and drops pending");
    }

    process.exit(fails);
});

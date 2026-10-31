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

    run("fn square(x) { return x * x; }");
    r = run("print(square(9));");
    ok(r.out.includes("81"), "wasm: definitions persist across calls");

    run("let c = 0;");
    run("fn bump() { c = c + 1; return c; }");
    run("bump(); bump();");
    r = run("print(bump());");
    ok(r.out.includes("3"), "wasm: closure over shared global");

    r = run("let xs = []; for (let i = 0; i < 5; i = i + 1) { push(xs, i * i); } print(xs);");
    ok(r.rc === 0 && r.out.includes("[0, 1, 4, 9, 16]"), "wasm: lists and loops");

    r = run("let x = ;");
    ok(r.rc !== 0 && r.err.length > 0, "wasm: compile error reported");
    r = run("print(1 + 1);");
    ok(r.rc === 0 && r.out.includes("2"), "wasm: session survives a compile error");

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

    process.exit(fails);
});

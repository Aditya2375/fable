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
    /* Marshal input with an explicit length: ccall's "string" conversion
     * would silently truncate at a NUL (round-13 F1). Output comes back as
     * raw bytes from the wrapper's combined tagged stream (round-20/21) -
     * decode UTF-8 non-fatally, exactly like the demo page. */
    const td = new TextDecoder("utf-8");
    /* round-21 F2: the wrapper hands over ONE combined stream of records -
     * [1 byte tag: 0 = stdout, 1 = stderr][4 bytes LE length][payload] - in
     * exact write order. Decode each record non-fatally, exactly like the
     * demo page, and keep the merged run list for order assertions. */
    const drain = () => {
        const len = m.ccall("fable_out_len", "number", [], []);
        const ptr = m.ccall("fable_out_ptr", "number", [], []);
        const buf = len > 0 ? m.HEAPU8.slice(ptr, ptr + len) : new Uint8Array(0);
        m.ccall("fable_out_drain", null, [], []);
        const merged = [];
        let i = 0;
        while (i + 5 <= buf.length) {
            const tag = buf[i];
            const n = buf[i+1] | (buf[i+2] << 8) | (buf[i+3] << 16) | (buf[i+4] << 24);
            i += 5;
            if (i + n > buf.length) break;
            if (merged.length && merged[merged.length - 1][0] === (tag === 1))
                merged[merged.length - 1][1] += td.decode(buf.subarray(i, i + n));
            else
                merged.push([tag === 1, td.decode(buf.subarray(i, i + n))]);
            i += n;
        }
        const o = merged.filter(r => !r[0]).map(r => r[1]).join("");
        const e = merged.filter(r => r[0]).map(r => r[1]).join("");
        return [o, e, merged];
    };
    const run = (src) => {
        const bytes = new TextEncoder().encode(src);
        const ptr = m._malloc(Math.max(bytes.length, 1));
        m.writeArrayToMemory(bytes, ptr);
        const rc = m.ccall("fable_exec_len", "number", ["number", "number"], [ptr, bytes.length]);
        m._free(ptr);
        const [o, e, merged] = drain();
        out = o; err = e;
        return { rc, out, err, merged };
    };
    ok(boot() === 0, "wasm: boot");

    let r = run("print(6 * 7);");
    ok(r.rc === 0 && r.out.includes("42"), "wasm: print works");

    /* round-20: the demo output path is byte-faithful. The CLI writes
     * 61 00 62 0a for print("a\0b") - one line with a NUL inside */
    r = run('print("a\\0b");');
    ok(r.out === "a\0b\n", "wasm: a printed NUL stays inside its line");
    r = run('print("héllo 漢字");');
    ok(r.out === "héllo 漢字\n", "wasm: valid UTF-8 prints unchanged");
    r = run('let bs = "abé漢"; print(bs[3]); print(bs[4]);');
    ok(r.out === "\uFFFD\n\uFFFD\n", "wasm: non-UTF-8 bytes decode to U+FFFD, never invented glyphs");

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
     * a silent truncation - all three critic repros, with absolute session
     * line numbers asserted exactly (round-17 F2) */
    boot();
    r = run("print(1);\0 @@@ garbage");
    ok(r.rc !== 0 && r.out === "" && r.err.includes("<web>:1: NUL byte in source"),
       "wasm: NUL after valid code rejects the whole input");
    r = run("\0let lost = 1;");
    ok(r.rc !== 0 && r.err.includes("<web>:2: NUL byte in source"),
       "wasm: leading NUL rejected, naming the absolute session line");
    r = run("print(lost);");
    ok(r.rc !== 0 && r.err.includes("<web>:3") && r.err.includes("undefined variable"),
       "wasm: nothing from a NUL-tainted line gets defined");
    r = run("print(2);");
    ok(r.rc === 0, "wasm: a good line after the drops");
    r = run("\0");
    ok(r.rc === 1 && r.err.startsWith("<web>:5: NUL byte in source"),
       "wasm: NUL after good lines names the absolute session line, bare like the CLI");
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

    /* round-18 F1: a trailing newline terminates the line instead of
     * consuming an extra session line (getline semantics) */
    boot();
    r = run("print(1);\n");
    ok(r.rc === 0 && r.out.includes("1"), "wasm: trailing newline runs the line");
    r = run("let q = ;");
    ok(r.rc === 1 && r.err.includes("<web>:2:"),
       "wasm: trailing newline did not consume a session line");

    /* round-18 F2: a multi-line call is processed per line with the CLI's
     * per-line cap - only the over-cap line is dropped, the rest still run */
    boot();
    run("fn f() {");
    r = run("/*" + "x".repeat(65520) + "*/");
    ok(r.rc === 2, "wasm: a line just under the cap is accepted (still pending)");
    r = run("}\nprint(9);");
    ok(r.rc === 1 && r.err.includes("input too long") && r.out.includes("9"),
       "wasm: multi-line call drops only the over-cap line and runs the rest");

    /* round-19: the 64 KB cap counts bytes exactly like getline - a
     * trailing newline counts, an unterminated tail has none to count */
    boot();
    r = run("#" + "x".repeat(65534));
    ok(r.rc === 0, "wasm: unterminated 65535-byte line accepted like the CLI");
    boot();
    r = run("#" + "x".repeat(65535));
    ok(r.rc === 1 && r.err.includes("input too long"),
       "wasm: unterminated 65536-byte line rejected like the CLI");
    boot();
    r = run("#" + "x".repeat(65533) + "\n");
    ok(r.rc === 0, "wasm: terminated 65535-byte line accepted");
    boot();
    r = run("#" + "x".repeat(65534) + "\n");
    ok(r.rc === 1 && r.err.includes("input too long"),
       "wasm: terminated 65536-byte line rejected");
    boot();
    run("if (1 < 2) {");
    r = run("#" + "x".repeat(65521));
    ok(r.rc === 2, "wasm: pending 13 + unterminated 65522-byte tail accepted");
    boot();
    run("if (1 < 2) {");
    r = run("#" + "x".repeat(65522));
    ok(r.rc === 1 && r.err.includes("input too long"),
       "wasm: pending 13 + unterminated 65523-byte tail rejected");

    /* round-17 F2: session differential - the same line sequence must name
     * the same absolute lines in the wasm session and the real CLI */
    const bin = require("path").join(__dirname, "..", "build", "fable");
    if (require("fs").existsSync(bin)) {
        const seq = ["print(1);", "\0", "print(2);", "fn g() {", "\0",
                     "let z = [", "nope];",
                     "let pad = \"" + "y".repeat(70000) + "\";", "print(3);"];
        boot();
        const wasmErr = seq.map(line => run(line).err).join("");
        const cli = require("child_process").spawnSync(bin, [],
            { input: seq.join("\n") + "\n", encoding: "utf8" });
        const lines = t => (t.match(/<(?:repl|web)>:(\d+)/g) || []).map(x => x.replace(/\D+/g, "")).join(",");
        ok(cli.status !== 0, "diff: the CLI exits nonzero on the NUL/too-long sequence");
        ok(lines(wasmErr) === lines(cli.stderr) && lines(wasmErr) === "2,5,7",
           "diff: wasm and the CLI name the same absolute lines for NUL/too-long sequences");
        ok(wasmErr.includes("input too long") && cli.stderr.includes("input too long"),
           "diff: both reject the over-64KB line");

        /* round-18 F2 differential: the straddling sequence behaves the same
         * on both sides, including the line after the dropped one */
        const seq2 = ["fn f() {", "/*" + "x".repeat(65520) + "*/", "}", "print(9);"];
        boot();
        let werr2 = "", wout2 = "";
        /* terminate each line like the CLI's stdin lines, so the byte counts
         * the cap checks are identical on both sides (round-19) */
        for (const line of seq2) { const rr = run(line + "\n"); werr2 += rr.err; wout2 += rr.out; }
        const cli2 = require("child_process").spawnSync(bin, [],
            { input: seq2.join("\n") + "\n", encoding: "utf8" });
        ok(werr2.includes("input too long") && cli2.stderr.includes("input too long"),
           "diff: both drop the straddling line");
        ok(wout2.includes("9") && cli2.stdout.includes("9"),
           "diff: both run the line after the dropped over-cap line");

        /* round-19 differential: the same cap boundaries at EOF, where the
         * CLI's final line has no newline either */
        let c = require("child_process").spawnSync(bin, [],
            { input: "#" + "x".repeat(65534), encoding: "utf8" });
        ok(c.status === 0, "diff: CLI accepts the unterminated 65535-byte line");
        c = require("child_process").spawnSync(bin, [],
            { input: "#" + "x".repeat(65535), encoding: "utf8" });
        ok(c.status !== 0 && c.stderr.includes("input too long"),
           "diff: CLI rejects the unterminated 65536-byte line");
        c = require("child_process").spawnSync(bin, [],
            { input: "if (1 < 2) {\n" + "#" + "x".repeat(65521), encoding: "utf8" });
        ok(!c.stderr.includes("input too long") && c.stderr.includes("unexpected end of input"),
           "diff: CLI accepts pending 13 + unterminated 65522-byte tail");
        c = require("child_process").spawnSync(bin, [],
            { input: "if (1 < 2) {\n" + "#" + "x".repeat(65522), encoding: "utf8" });
        ok(c.stderr.includes("input too long"),
           "diff: CLI rejects pending 13 + unterminated 65523-byte tail");

        /* round-20 differential: the CLI prints a NUL byte raw; the wasm
         * session delivers the same bytes */
        boot();
        const wn = run('print("a\\0b");');
        c = require("child_process").spawnSync(bin, [],
            { input: 'print("a\\0b");\n', encoding: null });
        ok(Buffer.from(c.stdout).equals(Buffer.from([0x61, 0x00, 0x62, 0x0a])),
           "diff: CLI prints the NUL byte raw (61 00 62 0a)");
        ok(wn.out === "a\0b\n", "diff: wasm delivers the same four bytes");

        /* round-21 F2: stdout/stderr interleave order survives one exec.
         * CLI with 2>&1 on a pipe: the unbuffered stderr error lands before
         * the buffered stdout flush at exit. The wasm session must deliver
         * the same order through its tagged records. */
        boot();
        const wi = run('badcall();\nprint("after");');
        ok(wi.merged.length === 2 && wi.merged[0][0] === true && wi.merged[1][0] === false
           && wi.merged[0][1].includes("<web>:1:") && wi.merged[1][1] === "after\n",
           "wasm: error then stdout, in write order, within one exec");
        c = require("child_process").spawnSync("sh", ["-c", '"' + bin + '" 2>&1'],
            { input: 'badcall();\nprint("after");\n', encoding: "utf8" });
        ok(c.stdout.indexOf("<repl>:1:") !== -1 && c.stdout.indexOf("after") > c.stdout.indexOf("<repl>:1:"),
           "diff: the CLI interleaves the same way (error before after)");
        ok(wi.merged.map(([, t]) => t).join("").replace(/<web>/, "<repl>") === c.stdout,
           "diff: wasm and CLI merged streams match exactly");
    } else {
        console.log("FAIL: build/fable missing - run make first for the CLI differential");
        fails = 1;
    }

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

    /* round-18 N2: 16px input font so iOS Safari does not auto-zoom on focus */
    ok(/#in \{[^}]*font:16px/.test(html), "demo: session input is 16px (no iOS auto-zoom)");

    /* round-20: the page renders VM bytes faithfully - split on LF only,
     * U+FFFD for non-UTF-8, ␀ for NUL on the same line */
    /* round-21 F1/F2: the page renderer - one DOM flush per drain, a DOM
     * cap with a trim marker, and in-order stdout/stderr runs. Extract the
     * real block (errNext..renderTagged) and drive it with a stub DOM. */
    const renderSrc = grab(/let errNext = false;[\s\S]*?\n\}\nfunction drainOutput/, "the renderer block");
    if (renderSrc) {
        const body = renderSrc.slice(0, renderSrc.lastIndexOf("function drainOutput"));
        let appends = 0, scrolls = 0;
        const nodes = [];
        const syncMarker = () => { if (markerNode) markerNode.nextSibling = nodes[nodes.indexOf(markerNode) + 1] || null; };
        let markerNode = null;
        const out = {
            appendChild(n) { appends++; if (n.children) nodes.push(...n.children); else nodes.push(n); },
            insertBefore(n) { appends++; nodes.unshift(n); n.parentNode = out; markerNode = n; syncMarker(); },
            removeChild(n) { nodes.splice(nodes.indexOf(n), 1); syncMarker(); },
            get firstChild() { return nodes[0] || null; },
            get scrollTop() { return 0; },
            set scrollTop(v) { scrollSets++; },
            scrollHeight: 100, textContent: ""
        };
        let scrollSets = 0;
        const document = {
            createElement: () => ({ className: "", textContent: "", parentNode: null, nextSibling: null }),
            createTextNode: t => ({ textContent: t }),
            createDocumentFragment: () => ({ children: [], appendChild(n) { this.children.push(n); } })
        };
        let __state = null, __reset = null;
        eval(body + "\n__state = () => ({ shownLines, trimmedLines });\n__reset = () => { shownLines = 0; trimmedLines = 0; trimMarker = null; };");
        const rec = (tag, s) => {
            const p = new TextEncoder().encode(s);
            const b = new Uint8Array(5 + p.length);
            b[0] = tag;
            b[1] = p.length & 255; b[2] = (p.length >> 8) & 255; b[3] = (p.length >> 16) & 255; b[4] = (p.length >> 24) & 255;
            b.set(p, 5);
            return b;
        };
        const cat = (...bs) => {
            const all = new Uint8Array(bs.reduce((n, b) => n + b.length, 0));
            let o = 0;
            for (const b of bs) { all.set(b, o); o += b.length; }
            return all;
        };
        /* byte-faithful shapes through the tagged protocol */
        appends = 0; scrollSets = 0;
        const recRaw = (tag, p) => {
            const b = new Uint8Array(5 + p.length);
            b[0] = tag;
            b[1] = p.length & 255; b[2] = (p.length >> 8) & 255; b[3] = (p.length >> 16) & 255; b[4] = (p.length >> 24) & 255;
            b.set(p, 5);
            return b;
        };
        renderTagged(cat(rec(0, "a\0b\n"), rec(0, "héllo 漢字\n"), recRaw(1, new Uint8Array([0x61, 0xa9, 0x0a]))));
        const text = nodes.map(n => n.textContent).join("");
        ok(text === "a␀b\nhéllo 漢字\na\uFFFD\n",
           "demo: tagged records render exactly - NUL in place, U+FFFD, UTF-8 unchanged");
        ok(appends === 1, "demo: one appendChild per drain, not one per line");
        ok(scrollSets === 1, "demo: one scroll per drain, not one per line");
        /* interleave order: err record between two out records stays in place */
        nodes.length = 0;
        renderTagged(cat(rec(0, "before\n"), rec(1, "oops\n"), rec(0, "after\n")));
        ok(nodes.map(n => n.textContent).join("") === "before\noops\nafter\n",
           "demo: stdout/stderr interleave order preserved (round-21 F2)");
        ok(nodes[1].className === "err", "demo: the error run keeps its styling");
        /* F1 perf + cap: 20000 printed lines in one drain */
        nodes.length = 0; appends = 0; scrollSets = 0;
        __reset();
        let big = "";
        for (let i = 0; i < 20000; i++) big += i + "\n";
        const t0 = Date.now();
        renderTagged(rec(0, big));
        const dt = Date.now() - t0;
        const st = __state();
        ok(dt < 5000, "demo: 20000-line drain renders fast (" + dt + " ms, budget 5000)");
        ok(appends <= 2, "demo: one append for 20000 lines (plus the trim marker)");
        ok(scrollSets === 1, "demo: one scroll for 20000 lines");
        ok(st.trimmedLines === 15000 && st.shownLines === 5000,
           "demo: DOM capped at 5000 lines with 15000 trimmed");
        ok(nodes.length === 11 && nodes[0].textContent.includes("15000 earlier lines trimmed"),
           "demo: a trim marker names the dropped line count, newest 5000 lines kept");
    }
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

    /* round-17 F1: the wrapper is the single source of truth for NUL
     * rejection, the input cap and session line numbering - the page must
     * not re-implement a belt whose counters can desync from the wrapper's */
    const execSrc = grab(/function exec\(src\)\{[\s\S]*?\n\}/, "exec()");
    ok(execSrc && !execSrc.includes("indexOf") && !html.includes("pendingLines"),
       "demo: exec() leaves NUL handling and line numbering to the wrapper");

    /* round-21 F5: an interpreter OOM (exit(70) from vm.c/gc.c) throws
     * ExitStatus through ccall; the page must end the session cleanly -
     * clear message, input locked - and "reset session" must revive it.
     * Extract the real exec() + runProgram() and drive them with stubs. */
    const execSrc2 = grab(/function exec\(src\)\{[\s\S]*?\n\}/, "exec()");
    const rpSrc = grab(/function runProgram\(name\)\{[\s\S]*?\n\}/, "runProgram()");
    if (execSrc2 && rpSrc) {   /* own scope: the stubs shadow the round-10 names */
        const inp2 = { disabled: false };
        const errs2 = [];
        const printErr2 = t => { errs2.push(t); };
        const printCmd2 = () => {};
        let bootCalls = 0, execCalls = 0;
        const M2 = {
            _malloc: () => 64,
            _free: () => {},
            writeArrayToMemory: () => {},
            HEAPU8: new Uint8Array(0),
            ccall: (name) => {
                if (name === "fable_boot") { bootCalls++; return 0; }
                if (name === "fable_exec_len") { execCalls++; throw { name: "ExitStatus", status: 70 }; }
                return 0;
            }
        };
        const drainOutput2 = () => {};
        let sessionDead = false;
        const inp = inp2, M = M2, printErr = printErr2, printCmd = printCmd2, drainOutput = drainOutput2;
        const setEcho = () => {};
        const programs = {};
        const fns = eval("(function(){" + execSrc2 + "\n" + rpSrc + "\nreturn { exec, runProgram }; })")();
        ok(fns.exec("print(1);") === 1, "demo: an ExitStatus exec reports an error");
        ok(sessionDead && inp.disabled, "demo: the session dies and the input locks");
        ok(errs2.length === 1 && errs2[0].includes("ran out of memory") && errs2[0].includes("reset session"),
           "demo: the OOM message says what happened and how to recover");
        ok(fns.exec("print(2);") === 1 && execCalls === 1, "demo: a dead session never reaches the interpreter");
        fns.runProgram("reset");
        ok(bootCalls === 1 && !sessionDead && !inp.disabled, "demo: reset session boots a fresh VM and unlocks");
        ok(fns.exec("print(3);") === 1 && execCalls === 2, "demo: the revived session reaches the interpreter again");
    }

    process.exit(fails);
});

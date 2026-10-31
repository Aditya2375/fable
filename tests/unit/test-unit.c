/* Unit checks: lexer, values, table, chunk, and small end-to-end runs
   through the public interpret() API. */
#include "vm.h"
#include "compiler.h"
#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long checks;
#define OK(cond) do { \
    checks++; \
    if (!(cond)) { fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } \
} while (0)

/* capture output of an interpret() run */
static char last_err[4096];

static char *run_capture(const char *src, InterpretResult *res) {
    char *buf = NULL;
    size_t cap = 0;
    char *ebuf = NULL;
    size_t ecap = 0;
    FILE *mem = open_memstream(&buf, &cap);
    FILE *memerr = open_memstream(&ebuf, &ecap);
    vm.out = mem;
    vm.err = memerr;
    InterpretResult r = interpret(src, "<test>");
    fclose(mem);
    fclose(memerr);
    snprintf(last_err, sizeof last_err, "%s", ebuf ? ebuf : "");
    free(ebuf);
    if (res) *res = r;
    return buf;
}

static void lexer_basics(void) {
    Lexer lx;
    lexer_init(&lx, "let x = 42; # note\nfn");
    OK(lexer_next(&lx).type == T_LET);
    OK(lexer_next(&lx).type == T_IDENT);
    OK(lexer_next(&lx).type == T_EQ);
    Token n = lexer_next(&lx);
    OK(n.type == T_NUMBER && n.len == 2 && n.start[0] == '4');
    OK(lexer_next(&lx).type == T_SEMICOLON);
    OK(lexer_next(&lx).type == T_FN);
    OK(lexer_next(&lx).type == T_EOF);

    lexer_init(&lx, "1.5e3 != nil and or not while for if else return true false");
    OK(lexer_next(&lx).type == T_NUMBER);
    OK(lexer_next(&lx).type == T_BANGEQ);
    OK(lexer_next(&lx).type == T_NIL);
    OK(lexer_next(&lx).type == T_AND);
    OK(lexer_next(&lx).type == T_OR);
    OK(lexer_next(&lx).type == T_NOT);
    OK(lexer_next(&lx).type == T_WHILE);
    OK(lexer_next(&lx).type == T_FOR);
    OK(lexer_next(&lx).type == T_IF);
    OK(lexer_next(&lx).type == T_ELSE);
    OK(lexer_next(&lx).type == T_RETURN);
    OK(lexer_next(&lx).type == T_TRUE);
    OK(lexer_next(&lx).type == T_FALSE);

    lexer_init(&lx, "\"a\\n\\t\\\"\\\\b\" @");
    Token s = lexer_next(&lx);
    OK(s.type == T_STRING);
    OK(lexer_next(&lx).type == T_ERROR);

    lexer_init(&lx, "\"unclosed");
    OK(lexer_next(&lx).type == T_ERROR);
}

/* Low-level object/table tests keep raw ObjString* with no GC roots, which
 * forced collection would free. They test the data structures, not the GC, so
 * run them with collection paused. */
static int pause_gc(void) { return vm.gc_paused++; }

static void resume_gc(int saved) { vm.gc_paused = saved; }

static void values_basics(void) {
    int gc_saved = pause_gc();
    OK(values_equal(NUM_VAL(1), NUM_VAL(1)));
    OK(!values_equal(NUM_VAL(1), NUM_VAL(2)));
    OK(!values_equal(NUM_VAL(1), BOOL_VAL(true)));
    OK(values_equal(NIL_VAL, NIL_VAL));
    OK(!value_truthy(NIL_VAL));
    OK(!value_truthy(BOOL_VAL(false)));
    OK(value_truthy(NUM_VAL(0)));    /* only nil/false are falsy */
    /* interning: identical strings are the same object */
    ObjString *a = string_copy("hello", 5);
    ObjString *b = string_copy("hello", 5);
    OK(a == b);
    ObjString *c = string_copy("world", 5);
    OK(a != c);
    OK(values_equal(OBJ_VAL(a), OBJ_VAL(b)));
    resume_gc(gc_saved);
}

static void table_basics(void) {
    int gc_saved = pause_gc();
    Table t; table_init(&t);
    ObjString *k1 = string_copy("alpha", 5);
    ObjString *k2 = string_copy("beta", 4);
    OK(table_set(&t, k1, NUM_VAL(1)));
    OK(table_set(&t, k2, NUM_VAL(2)));
    OK(!table_set(&t, k1, NUM_VAL(10)));   /* existing key */
    Value v;
    OK(table_get(&t, k1, &v) && AS_NUM(v) == 10);
    OK(table_delete(&t, k1));
    OK(!table_get(&t, k1, &v));
    OK(table_get(&t, k2, &v) && AS_NUM(v) == 2);
    /* fill + rehash */
    char buf[16];
    for (int i = 0; i < 200; i++) {
        snprintf(buf, sizeof buf, "key%d", i);
        ObjString *k = string_copy(buf, (int)strlen(buf));
        OK(table_set(&t, k, NUM_VAL(i)));
    }
    for (int i = 0; i < 200; i++) {
        snprintf(buf, sizeof buf, "key%d", i);
        ObjString *k = string_copy(buf, (int)strlen(buf));
        OK(table_get(&t, k, &v) && AS_NUM(v) == i);
    }
    table_free(&t);
    resume_gc(gc_saved);
}

static void chunk_basics(void) {
    Chunk c; chunk_init(&c);
    chunk_write(&c, OP_NIL, 1);
    chunk_write(&c, OP_RETURN, 2);
    int i = chunk_add_constant(&c, NUM_VAL(3.5));
    OK(i == 0);
    OK(chunk_add_constant(&c, NUM_VAL(3.5)) == 0);   /* dedupe */
    OK(chunk_add_constant(&c, NUM_VAL(4)) == 1);
    OK(c.count == 2 && c.lines[1] == 2);
    chunk_patch16(&c, 0, 0xBEEF);
    OK(c.code[0] == 0xBE && c.code[1] == 0xEF);
    chunk_free(&c);
}

static char *run_capture2(const char *src, InterpretResult *res, char **err_out) {
    char *buf = NULL;
    size_t cap = 0;
    char *ebuf = NULL;
    size_t ecap = 0;
    FILE *mem = open_memstream(&buf, &cap);
    FILE *memerr = open_memstream(&ebuf, &ecap);
    vm.out = mem;
    vm.err = memerr;
    InterpretResult r = interpret(src, "<test>");
    fclose(mem);
    fclose(memerr);
    if (res) *res = r;
    if (err_out) *err_out = ebuf;
    else free(ebuf);
    return buf;
}

/* critic round 1 regression tests */
static void critic_r1(void) {
    InterpretResult r;
    char *out, *err;

    /* HIGH1: indexing a freshly-concatenated string must not UAF under GC */
    out = run_capture("print((\"he\" + \"llo\")[2]);", &r);
    OK(r == INTERPRET_OK && strcmp(out, "l\n") == 0);
    free(out);
    out = run_capture("let s = \"abc\"; for (let i = 0; i < 3; i = i + 1) { print(s[i]); }", &r);
    OK(r == INTERPRET_OK && strcmp(out, "a\nb\nc\n") == 0);
    free(out);
    out = run_capture("print([\"x\", \"y\"] + [\"z\"]);", &r);
    OK(r == INTERPRET_OK && strcmp(out, "[\"x\", \"y\", \"z\"]\n") == 0);
    free(out);

    /* HIGH2: REPL echo stays at the top level of the script */
    vm.repl = true;
    out = run_capture("fn make_counter() {\n"
                      "    let n = 0;\n"
                      "    fn step() { n = n + 1; return n; }\n"
                      "    return step;\n"
                      "}\n"
                      "let c = make_counter();\n"
                      "print(c());\n"
                      "print(c());\n", &r);
    OK(r == INTERPRET_OK && strcmp(out, "1\n2\n") == 0);
    free(out);
    out = run_capture("let i = 0;\nwhile (i < 3) { print(i); i = i + 1; }\n", &r);
    OK(r == INTERPRET_OK && strcmp(out, "0\n1\n2\n") == 0);
    free(out);
    out = run_capture("for (let j = 0; j < 3; j = j + 1) { print(j * 10); }\n", &r);
    OK(r == INTERPRET_OK && strcmp(out, "0\n10\n20\n") == 0);
    free(out);
    vm.repl = false;

    /* HIGH3: cyclic and deeply nested lists print safely */
    out = run_capture("let l = []; push(l, l); print(l);", &r);
    OK(r == INTERPRET_OK && strcmp(out, "[[...]]\n") == 0);
    free(out);
    out = run_capture("let l = []; push(l, l); print(str(l));", &r);
    OK(r == INTERPRET_OK && strcmp(out, "[[...]]\n") == 0);
    free(out);
    out = run_capture("let a = []; push(a, a); let b = []; push(b, a); push(b, a); print(b);", &r);
    OK(r == INTERPRET_OK && strcmp(out, "[[[...]], [[...]]]\n") == 0);
    free(out);
    out = run_capture("let l = 0; for (let i = 0; i < 100; i = i + 1) { l = [l]; } print(l); print(\"alive\");", &r);
    OK(r == INTERPRET_OK && strstr(out, "...") != NULL && strstr(out, "alive") != NULL);
    free(out);

    /* MED4: real <= and >=, IEEE NaN semantics */
    out = run_capture("print(1 <= 2); print(2 <= 2); print(3 <= 2);", &r);
    OK(strcmp(out, "true\ntrue\nfalse\n") == 0);
    free(out);
    out = run_capture("print(1 >= 2); print(2 >= 2); print(3 >= 2);", &r);
    OK(strcmp(out, "false\ntrue\ntrue\n") == 0);
    free(out);
    out = run_capture("print(0/0 <= 1); print(0/0 >= 1); print(0/0 < 1); print(0/0 > 1); print(0/0 == 0/0);", &r);
    OK(strcmp(out, "false\nfalse\nfalse\nfalse\nfalse\n") == 0);
    free(out);

    /* MED5: huge doubles never hit UB casts */
    out = run_capture("print(1e300); print(-1e300); print(str(1e300));", &r);
    OK(r == INTERPRET_OK && strcmp(out, "1e+300\n-1e+300\n1e+300\n") == 0);
    free(out);
    out = run_capture2("let l = [1]; print(l[1e300]);", &r, &err);
    OK(r == INTERPRET_RUNTIME_ERROR && strstr(err, "out of range") != NULL);
    free(out); free(err);
    out = run_capture2("print(\"ab\"[1e300]);", &r, &err);
    OK(r == INTERPRET_RUNTIME_ERROR && strstr(err, "out of range") != NULL);
    free(out); free(err);
    out = run_capture2("print(\"ab\"[-1e300]);", &r, &err);
    OK(r == INTERPRET_RUNTIME_ERROR && strstr(err, "out of range") != NULL);
    free(out); free(err);

    /* MED6: deep nesting is a clean compile error */
    {
        static char deep[3000];
        int off = 0;
        for (int i = 0; i < 600; i++) off += snprintf(deep + off, sizeof deep - (size_t)off, "(");
        off += snprintf(deep + off, sizeof deep - (size_t)off, "1");
        for (int i = 0; i < 600; i++) off += snprintf(deep + off, sizeof deep - (size_t)off, ")");
        snprintf(deep + off, sizeof deep - (size_t)off, ";");
        out = run_capture2(deep, &r, &err);
        OK(r == INTERPRET_COMPILE_ERROR && strstr(err, "nested too deeply") != NULL);
        free(out); free(err);
    }
    {
        static char deepb[8000];
        int off = snprintf(deepb, sizeof deepb, "if (true) {\n");
        for (int i = 0; i < 300; i++) off += snprintf(deepb + off, sizeof deepb - (size_t)off, "if (true) {\n");
        off += snprintf(deepb + off, sizeof deepb - (size_t)off, "print(1);\n");
        for (int i = 0; i < 301; i++) off += snprintf(deepb + off, sizeof deepb - (size_t)off, "}\n");
        out = run_capture2(deepb, &r, &err);
        OK(r == INTERPRET_COMPILE_ERROR && strstr(err, "nested too deeply") != NULL);
        free(out); free(err);
    }

    /* MED7: recursion works well past 64 frames and fails gracefully */
    out = run_capture("fn f(n) { if (n == 0) { return 0; } return f(n - 1) + 1; } print(f(900));", &r);
    OK(r == INTERPRET_OK && strcmp(out, "900\n") == 0);
    free(out);
    out = run_capture2("fn f(n) { if (n == 0) { return 0; } return f(n - 1) + 1; } print(f(5000));", &r, &err);
    OK(r == INTERPRET_RUNTIME_ERROR && strstr(err, "stack overflow") != NULL);
    free(out); free(err);

    /* MED8: -disasm dumps bytecode */
    vm.disasm = true;
    out = run_capture("print(1 + 2);", &r);
    OK(r == INTERPRET_OK && strstr(out, "OP_ADD") != NULL && strstr(out, "3") != NULL);
    free(out);
    vm.disasm = false;

    /* MED9: source completeness scan for REPL continuation */
    OK(fable_source_complete("print(1);") == true);
    OK(fable_source_complete("fn f() {") == false);
    OK(fable_source_complete("fn f() {}") == true);
    OK(fable_source_complete("let x = [1, 2") == false);
    OK(fable_source_complete("let x = [1, 2]") == true);
    OK(fable_source_complete("\"abc") == false);
    OK(fable_source_complete("\"a{b}c\"") == true);
    OK(fable_source_complete("/* never") == false);
    OK(fable_source_complete("/* fine */ print(1);") == true);
    OK(fable_source_complete("let s = \"}\";") == true);

    /* LOW13: long number literals are not truncated */
    out = run_capture("print(10000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000);", &r);
    OK(r == INTERPRET_OK && strcmp(out, "1e+100\n") == 0);
    free(out);

    /* LOW14: unterminated block comment is a compile error */
    out = run_capture2("print(1); /* never closed", &r, &err);
    OK(r == INTERPRET_COMPILE_ERROR && strstr(err, "unterminated block comment") != NULL);
    free(out); free(err);

    /* LOW16: errors name the source */
    out = run_capture2("let x = ;", &r, &err);
    OK(r == INTERPRET_COMPILE_ERROR && strstr(err, "<test>:1:") != NULL);
    free(out); free(err);
    out = run_capture2("print(undefined_thing);", &r, &err);
    OK(r == INTERPRET_RUNTIME_ERROR && strstr(err, "<test>:1:") != NULL);
    free(out); free(err);

    /* LOW17: negative zero keeps its sign */
    out = run_capture("print(-0.0); print(0.0); print(-0.0 == 0.0);", &r);
    OK(strcmp(out, "-0\n0\ntrue\n") == 0);
    free(out);

    /* LOW18: strings with embedded NUL print in full */
    {
        int paused = pause_gc();
        ObjString *nul = string_copy("a\0b", 3);
        char *buf = NULL;
        size_t cap = 0;
        FILE *mem = open_memstream(&buf, &cap);
        value_print(mem, OBJ_VAL(nul));
        fclose(mem);
        OK(cap == 3 && memcmp(buf, "a\0b", 3) == 0);
        free(buf);
        resume_gc(paused);
    }

    /* LOW19: num(str(x)) round-trips exactly */
    out = run_capture("print(num(str(0.1)) == 0.1); print(num(str(1/3)) == 1/3); print(num(str(1e300)) == 1e300); print(num(str(-0.0)) == 0);", &r);
    OK(strcmp(out, "true\ntrue\ntrue\ntrue\n") == 0);
    free(out);
    out = run_capture("print(0.1 + 0.2); print(2/3);", &r);
    OK(strcmp(out, "0.30000000000000004\n0.6666666666666666\n") == 0);
    free(out);

    /* round-7 F3: integral values with magnitude at most 2^53 print as
     * plain integers (the critic's case was 1694577218885960); every
     * printed form still round-trips through num(str(x)) */
    out = run_capture("print(1694577218885960); print(-1694577218885960); print(999999999999999); print(1000000000000000); print(9007199254740992);", &r);
    OK(strcmp(out, "1694577218885960\n-1694577218885960\n999999999999999\n1000000000000000\n9007199254740992\n") == 0);
    free(out);
    out = run_capture("print(num(str(1694577218885960)) == 1694577218885960); print(num(str(9007199254740992)) == 9007199254740992); print(num(str(9007199254740994)) == 9007199254740994); print(num(str(1e20)) == 1e20);", &r);
    OK(strcmp(out, "true\ntrue\ntrue\ntrue\n") == 0);
    free(out);

    /* LOW20: str(list) renders exactly like print(list) */
    out = run_capture("print(str([1, \"a\", 2.5, true, nil]));", &r);
    OK(strcmp(out, "[1, \"a\", 2.5, true, nil]\n") == 0);
    free(out);
    out = run_capture("let l = []; push(l, l); print(str(l) == \"[[...]]\");", &r);
    OK(strcmp(out, "true\n") == 0);
    free(out);
}

/* critic round 2 regression tests */
static void critic_r2(void) {
    InterpretResult r;
    char *out, *err;

    /* HIGH1: nested fn declarations are depth-guarded, not a C-stack crash */
    {
        char *src = malloc(2048);
        int off = 0;
        for (int i = 0; i < 200; i++) off += snprintf(src + off, 2048 - (size_t)off, "fn a(){");
        for (int i = 0; i < 200; i++) off += snprintf(src + off, 2048 - (size_t)off, "}");
        (void)off;
        out = run_capture(src, &r);
        OK(r == INTERPRET_OK);   /* 200 nested declarations work */
        free(out); free(src);
    }
    {
        size_t n = 100000;
        size_t sz = n * 8 + 8;
        char *src = malloc(sz);
        int off = 0;
        for (size_t i = 0; i < n; i++) off += snprintf(src + off, sz - (size_t)off, "fn a(){");
        for (size_t i = 0; i < n; i++) off += snprintf(src + off, sz - (size_t)off, "}");
        (void)off;
        out = run_capture2(src, &r, &err);
        OK(r == INTERPRET_COMPILE_ERROR);
        OK(strstr(err, "functions nested too deeply") != NULL);
        /* LOW4: exactly one error line, no cascade */
        OK(strchr(err, '\n') != NULL && strchr(strchr(err, '\n') + 1, '\n') == NULL);
        free(out); free(err); free(src);
    }
    /* unbraced statement chains hit their own guard */
    {
        char *src = malloc(8192);
        int off = 0;
        for (int i = 0; i < 300; i++) off += snprintf(src + off, 8192 - (size_t)off, "if (true) ");
        snprintf(src + off, 8192 - (size_t)off, "print(1);");
        out = run_capture2(src, &r, &err);
        OK(r == INTERPRET_COMPILE_ERROR && strstr(err, "statements nested too deeply") != NULL);
        free(out); free(err); free(src);
    }

    /* MED1: REPL echo stays out of unbraced bodies too */
    vm.repl = true;
    out = run_capture("let i = 0; while (i < 3) i = i + 1; print(i);", &r);
    OK(r == INTERPRET_OK && strcmp(out, "3\n") == 0);
    free(out);
    out = run_capture("if (true) 42;", &r);
    OK(r == INTERPRET_OK && strcmp(out, "") == 0);
    free(out);
    out = run_capture("if (false) 42; else 43;", &r);
    OK(r == INTERPRET_OK && strcmp(out, "") == 0);
    free(out);
    out = run_capture("if (true) if (true) 42;", &r);
    OK(r == INTERPRET_OK && strcmp(out, "") == 0);
    free(out);
    out = run_capture("for (let i = 0; i < 3; i = i + 1) i * 10;", &r);
    OK(r == INTERPRET_OK && strcmp(out, "") == 0);
    free(out);
    out = run_capture("42;", &r);
    OK(r == INTERPRET_OK && strcmp(out, "42\n") == 0);   /* top level still echoes */
    free(out);
    vm.repl = false;

    /* LOW1: fractional string index gets the list-style message */
    out = run_capture2("print(\"ab\"[1.5]);", &r, &err);
    OK(r == INTERPRET_RUNTIME_ERROR && strstr(err, "string index 1.5 out of range (len 2)") != NULL);
    free(out); free(err);

    /* LOW3: deep-recursion traces are capped */
    out = run_capture2("fn f(n) { if (n == 0) { return 0; } return f(n - 1) + 1; } print(f(5000));", &r, &err);
    OK(r == INTERPRET_RUNTIME_ERROR && strstr(err, "more frames") != NULL);
    {
        int lines = 0;
        for (char *p = err; *p; p++) if (*p == '\n') lines++;
        OK(lines < 20);
    }
    free(out); free(err);

    /* LOW5: num() is strict */
    out = run_capture2("print(num(\" 12\"));", &r, &err);
    OK(r == INTERPRET_RUNTIME_ERROR && strstr(err, "not a number") != NULL);
    free(out); free(err);
    out = run_capture2("print(num(\"0x10\"));", &r, &err);
    OK(r == INTERPRET_RUNTIME_ERROR && strstr(err, "not a number") != NULL);
    free(out); free(err);
    out = run_capture2("print(num(\"12 \"));", &r, &err);
    OK(r == INTERPRET_RUNTIME_ERROR && strstr(err, "not a number") != NULL);
    free(out); free(err);
    out = run_capture("print(num(\"12\")); print(num(\"inf\")); print(num(\"-0\")); print(num(\"1e3\"));", &r);
    OK(r == INTERPRET_OK && strcmp(out, "12\ninf\n-0\n1000\n") == 0);
    free(out);
}

static void repl_echo(void) {
    /* REPL mode echoes expression statements, but nil stays silent so
     * print() calls don't trail a nil. */
    vm.repl = true;
    InterpretResult r;
    char *out = run_capture("1 + 2;", &r);
    OK(r == INTERPRET_OK);
    OK(strcmp(out, "3\n") == 0);
    free(out);
    out = run_capture("print(\"x\");", &r);
    OK(r == INTERPRET_OK);
    OK(strcmp(out, "x\n") == 0);
    free(out);
    out = run_capture("\"bare\";", &r);
    OK(strcmp(out, "bare\n") == 0);
    free(out);
    vm.repl = false;
}

static void end_to_end(void) {
    InterpretResult r;
    char *out;

    out = run_capture("print(1 + 2 * 3);", &r);
    OK(r == INTERPRET_OK && !strcmp(out, "7\n"));
    free(out);

    out = run_capture("print(\"a\" + \"b\");", &r);
    OK(!strcmp(out, "ab\n"));
    free(out);

    out = run_capture("let x = 5; x = x * 2; print(x);", &r);
    OK(!strcmp(out, "10\n"));
    free(out);

    out = run_capture("print(3 > 2); print(3 <= 2); print(1 == 1); print(1 != 1);", &r);
    OK(!strcmp(out, "true\nfalse\ntrue\nfalse\n"));
    free(out);

    out = run_capture("print(nil == nil); print(nil == 0);", &r);
    OK(!strcmp(out, "true\nfalse\n"));
    free(out);

    out = run_capture("print(7 % 3); print(10 / 4); print(-3);", &r);
    OK(!strcmp(out, "1\n2.5\n-3\n"));
    free(out);

    out = run_capture("if (1 < 2) print(\"yes\"); else print(\"no\");", &r);
    OK(!strcmp(out, "yes\n"));
    free(out);

    out = run_capture("let i = 0; let s = 0; while (i < 5) { s = s + i; i = i + 1; } print(s);", &r);
    OK(!strcmp(out, "10\n"));
    free(out);

    out = run_capture("let t = 0; for (let i = 1; i <= 4; i = i + 1) t = t + i; print(t);", &r);
    OK(!strcmp(out, "10\n"));
    free(out);

    out = run_capture("print(true and \"kept\"); print(false and 1); print(false or \"or-ed\");", &r);
    OK(!strcmp(out, "kept\nfalse\nor-ed\n"));
    free(out);

    /* functions + closures */
    out = run_capture("fn add(a, b) { return a + b; } print(add(3, 4));", &r);
    OK(!strcmp(out, "7\n"));
    free(out);

    out = run_capture(
        "fn counter() {\n"
        "  let n = 0;\n"
        "  return fn() { n = n + 1; return n; };\n"
        "}\n"
        "let c = counter();\n"
        "c(); c();\n"
        "print(c());\n", &r);
    OK(r == INTERPRET_OK && !strcmp(out, "3\n"));
    free(out);

    /* recursion (declaration binds its own name) */
    out = run_capture("fn fib(n) { if (n < 2) return n; return fib(n-1) + fib(n-2); } print(fib(15));", &r);
    OK(!strcmp(out, "610\n"));
    free(out);

    /* lists */
    out = run_capture(
        "let l = [1, 2, 3];\n"
        "print(len(l));\n"
        "print(l[0] + l[2]);\n"
        "l[1] = 99;\n"
        "print(l);\n"
        "print(l[-1]);\n"
        "push(l, 4);\n"
        "print(pop(l));\n"
        "print(l);\n", &r);
    OK(r == INTERPRET_OK && !strcmp(out, "3\n4\n[1, 99, 3]\n3\n4\n[1, 99, 3]\n"));
    free(out);

    out = run_capture("print([1,2] + [3]); print(\"abc\"[1]); print(len(\"hello\"));", &r);
    OK(!strcmp(out, "[1, 2, 3]\nb\n5\n"));
    free(out);

    /* strings repr inside lists */
    out = run_capture("print([\"a\", \"b\\n\"]);", &r);
    OK(!strcmp(out, "[\"a\", \"b\\n\"]\n"));
    free(out);

    /* error paths */
    out = run_capture("print(undefined_name);", &r);
    OK(r == INTERPRET_RUNTIME_ERROR);
    free(out);

    out = run_capture("print(1 + \"s\");", &r);
    OK(r == INTERPRET_RUNTIME_ERROR);
    free(out);

    out = run_capture("let l = [1]; print(l[5]);", &r);
    OK(r == INTERPRET_RUNTIME_ERROR);
    free(out);

    out = run_capture("print(1 +);", &r);
    OK(r == INTERPRET_COMPILE_ERROR);
    free(out);

    out = run_capture("fn f(a) {} f(1, 2);", &r);
    OK(r == INTERPRET_RUNTIME_ERROR);
    free(out);

    /* division semantics: IEEE inf, but modulo by zero is an error */
    out = run_capture("print(1 / 0);", &r);
    OK(r == INTERPRET_OK && !strcmp(out, "inf\n"));
    free(out);
    out = run_capture("print(1 % 0);", &r);
    OK(r == INTERPRET_RUNTIME_ERROR);
    free(out);

    /* closures survive scope exit (upvalues close over) */
    out = run_capture(
        "let f = nil;\n"
        "{ let x = \"captured\"; f = fn() { return x; }; }\n"
        "print(f());\n", &r);
    OK(r == INTERPRET_OK && !strcmp(out, "captured\n"));
    free(out);

    /* two closures sharing one variable */
    out = run_capture(
        "fn make() {\n"
        "  let n = 0;\n"
        "  return [fn() { n = n + 1; return n; }, fn() { return n; }];\n"
        "}\n"
        "let p = make();\n"
        "p[0](); p[0]();\n"
        "print(p[1]());\n", &r);
    OK(r == INTERPRET_OK && !strcmp(out, "2\n"));
    free(out);
}


/* Build "<prefix>nil, nil, ...<suffix>" with n nils without O(n^2) strcat. */
static char *src_with_nils(const char *prefix, int nils, const char *suffix) {
    size_t cap = strlen(prefix) + (size_t)nils * 6 + strlen(suffix) + 1;
    char *s = malloc(cap), *w = s;
    w += sprintf(w, "%s", prefix);
    for (int i = 0; i < nils; i++) w += sprintf(w, i ? ", nil" : "nil");
    sprintf(w, "%s", suffix);
    return s;
}

/* "fill(F)" fills the value stack to a chosen depth: every fill() frame
 * holds callee + arg + 151 locals = 153 slots, so after the script closure
 * F frames leave top = 1 + 153*F and the leaf call adds callee + arg = 2
 * more; leaf_locals lets then set the exact top before leaf_tail runs. */
static char *src_fullstack(int frames, int leaf_locals, const char *leaf_tail) {
    size_t cap = 256 + (size_t)(151 + leaf_locals + 4) * 18 + strlen(leaf_tail);
    char *s = malloc(cap), *w = s;
    w += sprintf(w, "fn fill(n) {\n if (n > 0) {\n");
    for (int i = 0; i < 151; i++) w += sprintf(w, "  let a%d = 0;\n", i);
    w += sprintf(w, "  fill(n - 1);\n } else {\n");
    for (int i = 0; i < leaf_locals; i++) w += sprintf(w, "  let b%d = 0;\n", i);
    w += sprintf(w, "  %s\n }\n}\nfill(%d);", leaf_tail, frames);
    return s;
}

/* Build "<open>"*depth + "1" + "<close>"*depth + ";" */
static char *src_nested(const char *open, const char *close, int depth) {
    size_t cap = (size_t)depth * (strlen(open) + strlen(close)) + 8;
    char *s = malloc(cap), *w = s;
    for (int i = 0; i < depth; i++) w += sprintf(w, "%s", open);
    w += sprintf(w, "1");
    for (int i = 0; i < depth; i++) w += sprintf(w, "%s", close);
    sprintf(w, ";");
    return s;
}

static void critic_r3(void) {
    InterpretResult r;
    char *out, *src;

    /* Round-3 HIGH (rebased round-7 F1): shield pushes on an exactly-full
     * value stack (65536 values) must stop execution before any further
     * allocation touches the unrooted object. List literals now build
     * incrementally (constant stack use at any length), so the stack is
     * filled with frames instead: each fill() frame holds callee + arg +
     * 151 locals = 153 slots, so the script closure plus 427 frames leaves
     * top = 65332 and the leaf frame's callee + arg make 65334; leaf
     * locals then set the exact pre-op top. Each case must fail with one
     * clean stack-overflow error; under GC stress + ASan the pre-fix
     * versions were heap-use-after-frees. */
    struct { const char *name; int leaf_locals; const char *tail; } full[] = {
        /* OP_BUILD_LIST: top 65536, the list's own push overflows */
        { "list-build", 202, "let x = [];" },
        /* OP_ADD concat: two empty lists fill 65536, the concat shield overflows */
        { "list-concat", 200, "let x = [] + [];" },
        /* push(): the callee fills 65536, the [] build overflows */
        { "native-push", 201, "let x = push([], 1);" },
        /* OP_CLOSURE: local q fills 65536, the closure shield overflows
         * before capture_upvalue allocates */
        { "closure", 201, "let q = 1; let f = fn() { return q; };" },
    };
    for (int i = 0; i < 4; i++) {
        src = src_fullstack(427, full[i].leaf_locals, full[i].tail);
        out = run_capture(src, &r);
        OK(r == INTERPRET_RUNTIME_ERROR);           /* full stack: clean error */
        OK(strstr(vm.errbuf, "stack overflow") != NULL);
        free(out); free(src);
    }
    /* one slot below full the same build runs fine (65536th value fits) */
    src = src_fullstack(427, 201, "let x = [];");
    out = run_capture(src, &r);
    OK(r == INTERPRET_OK);
    free(out); free(src);
    /* with frame headroom: push() and concat allocate (GC-stress
     * collected) and a print still fits */
    src = src_fullstack(426, 202, "let x = push([], 1); let y = [1, 2] + [3]; print(len(y));");
    out = run_capture(src, &r);
    OK(r == INTERPRET_OK && !strcmp(out, "3\n"));
    free(out); free(src);

    /* Round-3 F5: source ending inside a comment with open brackets is
     * incomplete (the REPL must keep reading, then error at EOF) */
    OK(!fable_source_complete("{\n# c"));
    OK(!fable_source_complete("{\n// c"));
    OK(fable_source_complete("{\n# c\n}"));
    OK(!fable_source_complete("/* never closed"));
    OK(fable_source_complete("/* closed */ { }"));

    /* Round-3 F6a: expression nesting is uniformly 512 for every form */
    const char *forms[3][2] = { { "[", "]" }, { "(", ")" }, { "-", "" } };
    for (int i = 0; i < 3; i++) {
        src = src_nested(forms[i][0], forms[i][1], 512);
        out = run_capture(src, &r);
        OK(r == INTERPRET_OK);
        free(out); free(src);
        src = src_nested(forms[i][0], forms[i][1], 513);
        out = run_capture(src, &r);
        OK(r == INTERPRET_COMPILE_ERROR);
        free(out); free(src);
    }

    /* Round-3 F6c: str() matches print() for functions and natives */
    out = run_capture("fn foo() {} print(str(foo));", &r);
    OK(r == INTERPRET_OK && !strcmp(out, "<fn foo>\n"));
    free(out);
    out = run_capture("print(str(len));", &r);
    OK(r == INTERPRET_OK && !strcmp(out, "<native len>\n"));
    free(out);
}


/* ---- critic round 4 ---- */
static void critic_r4(void) {
    InterpretResult r;
    char *out, *src;

    /* L1: "at end" errors point at the last real token's line in every
     * frontend - a trailing newline no longer shifts the number. */
    out = run_capture("1 + 1\n", &r);
    OK(r == INTERPRET_COMPILE_ERROR && strstr(last_err, "<test>:1: at end") != NULL);
    free(out);
    out = run_capture("1 +\n\n\n", &r);
    OK(r == INTERPRET_COMPILE_ERROR && strstr(last_err, "<test>:1: at end") != NULL);
    free(out);
    out = run_capture("let x = 1;\nlet y = 2;\n1 + 1\n", &r);
    OK(r == INTERPRET_COMPILE_ERROR && strstr(last_err, "<test>:3: at end") != NULL);
    free(out);

    /* L2: num() is exactly the source-literal grammar: no '+', digits on
     * both sides of the point; inf/nan words stay for round-tripping. */
    out = run_capture("num(\"+1\");", &r);
    OK(r == INTERPRET_RUNTIME_ERROR);
    free(out);
    out = run_capture("num(\"1.\");", &r);
    OK(r == INTERPRET_RUNTIME_ERROR);
    free(out);
    out = run_capture("num(\".5\");", &r);
    OK(r == INTERPRET_RUNTIME_ERROR);
    free(out);
    out = run_capture("print(num(\"-1\") + 2);", &r);
    OK(r == INTERPRET_OK && !strcmp(out, "1\n"));
    free(out);
    out = run_capture("print(num(\"1.5e3\") + num(\"-inf\"));", &r);
    OK(r == INTERPRET_OK && !strcmp(out, "-inf\n"));
    free(out);

    /* L3: nesting is uniform - every construct (parens, list brackets,
     * unary operators) costs exactly one of the 512 levels, mixed or not. */
    src = src_nested("-(", ")", 256);   /* 512 constructs */
    out = run_capture(src, &r);
    OK(r == INTERPRET_OK);
    free(out); free(src);
    src = src_nested("-(", ")", 257);   /* 514 constructs */
    out = run_capture(src, &r);
    OK(r == INTERPRET_COMPILE_ERROR);
    free(out); free(src);

    /* Info: the GC threshold tracks the live heap. A long session that
     * generates ~9 MB of garbage must end with a small live-byte counter
     * and a sane threshold - not a ratchet of total-ever-allocated. */
    out = run_capture(
        "for (let i = 0; i < 1000; i = i + 1) {\n"
        "  let junk = [];\n"
        "  for (let j = 0; j < 100; j = j + 1) { push(junk, \"abcdefghijklmnopqrstuvwxyz\" + str(j)); }\n"
        "}\n"
        "print(\"done\");\n", &r);
    OK(r == INTERPRET_OK && !strcmp(out, "done\n"));
    OK(vm.bytes_allocated < 3 * 1024 * 1024);
    OK(vm.next_gc <= 4 * GC_THRESHOLD);
    free(out);
}


/* ---- critic round 5 ---- */
/* "("*n + "7" + ")"*n: an expression n levels deep */
static char *src_parens(int n) {
    char *s = malloc((size_t)n * 2 + 2), *w = s;
    for (int i = 0; i < n; i++) *w++ = '(';
    *w++ = '7';
    for (int i = 0; i < n; i++) *w++ = ')';
    *w = '\0';
    return s;
}
/* "fn g<i>() { " repeated n times, then "x;", then n closing braces */
static char *src_nested_fns(int n) {
    size_t cap = (size_t)n * 24 + 8;
    char *s = malloc(cap), *w = s;
    for (int i = 0; i < n; i++) w += sprintf(w, "fn g%d() { ", i);
    w += sprintf(w, "x;");
    for (int i = 0; i < n; i++) *w++ = '}';
    *w = '\0';
    return s;
}

/* HIGH1: when a depth guard fires mid-parse, every parse loop must exit -
 * pre-fix the function-body loop called declaration() forever (100% CPU,
 * the wasm demo froze the tab). Each case is a guard in a different
 * position: inside a fn body (middle and last statement), a nested block,
 * a top-level block, call arguments, a list literal, a return value, loop
 * and for-clause expressions, an unbraced statement chain, deep blocks and
 * deep fn declarations. Every one must end in ONE clean "too deeply"
 * compile error, and the VM must stay usable for the next program. The
 * shell suite and fuzz tripwires guard these same programs with a
 * wall-clock timeout, so a regression hangs CI loudly instead of silently
 * spinning. */
static void critic_r5(void) {
    InterpretResult r;
    char *out, *src;
    char *p513 = src_parens(513);   /* one level over EXPR_DEPTH_MAX */

    const char *expr_cases[][3] = {
        /* prefix, guard tripper position, suffix */
        { "fn f() { let x = ", NULL, "; y; }" },              /* fn body, middle */
        { "fn f() { y; let x = ", NULL, "; }" },              /* fn body, last */
        { "fn f() { { let x = ", NULL, "; } print(1); }" },   /* nested block in fn */
        { "{ let x = ", NULL, "; } print(1);" },              /* top-level block */
        { "fn g(a) { return a; } fn f() { let x = g(", NULL, "); y; }" }, /* call args */
        { "fn f() { let xs = [", NULL, "]; y; }" },           /* list literal */
        { "fn f() { let y = 1; return ", NULL, "; }" },       /* return value */
        { "fn f() { while (", NULL, ") { } y; }" },           /* while condition */
        { "fn f() { for (let i = ", NULL, "; i < 3; i = i + 1) { } y; }" }, /* for clause */
    };
    for (size_t i = 0; i < sizeof expr_cases / sizeof expr_cases[0]; i++) {
        size_t cap = strlen(expr_cases[i][0]) + strlen(expr_cases[i][2]) + strlen(p513) + 1;
        src = malloc(cap);
        snprintf(src, cap, "%s%s%s", expr_cases[i][0], p513, expr_cases[i][2]);
        out = run_capture(src, &r);
        OK(r == INTERPRET_COMPILE_ERROR);
        OK(strstr(last_err, "expression nested too deeply") != NULL);
        free(out); free(src);
        out = run_capture("print(\"alive\");", &r);   /* VM is not wedged */
        OK(r == INTERPRET_OK && !strcmp(out, "alive\n"));
        free(out);
    }

    /* repro B from the review: an error mid-fn-body recovers and terminates */
    out = run_capture("fn f() { if (true) x257 print(1); print(2); }", &r);
    OK(r == INTERPRET_COMPILE_ERROR);
    free(out);

    /* statement chains (unbraced if/while), blocks and fn declarations: the
     * same termination guarantee for the other three depth guards, each
     * with a following statement that must not spin */
    {
        char chain[260 * 10 + 64];
        char *w = chain;
        w += sprintf(w, "fn f() { ");
        for (int i = 0; i < 260; i++) w += sprintf(w, "if (1) ");
        w += sprintf(w, "x; print(1); }");
        out = run_capture(chain, &r);
        OK(r == INTERPRET_COMPILE_ERROR);
        OK(strstr(last_err, "statements nested too deeply") != NULL);
        free(out);
    }
    {
        size_t cap = 2 * 300 + 64;
        src = malloc(cap);
        char *w = src;
        w += sprintf(w, "fn f() { ");
        for (int i = 0; i < 300; i++) *w++ = '{';
        for (int i = 0; i < 300; i++) *w++ = '}';
        sprintf(w, " print(1); }");
        out = run_capture(src, &r);
        OK(r == INTERPRET_COMPILE_ERROR);
        OK(strstr(last_err, "blocks nested too deeply") != NULL);
        free(out); free(src);
    }
    src = src_nested_fns(300);
    out = run_capture(src, &r);
    OK(r == INTERPRET_COMPILE_ERROR);
    OK(strstr(last_err, "functions nested too deeply") != NULL);
    free(out); free(src);
    out = run_capture("print(\"alive\");", &r);
    OK(r == INTERPRET_OK && !strcmp(out, "alive\n"));
    free(out);

    /* the budget is exact: 512 constructs pass at top level, a call or list
     * layer costs one level (511 inside), 513 always fails */
    src = src_parens(511);
    {   /* one call layer around 511 parens: exactly at the limit */
        size_t cap = strlen(src) + 64;
        char *w2 = malloc(cap);
        snprintf(w2, cap, "fn g(a) { return a; } let r = g(%s); print(r);", src);
        out = run_capture(w2, &r);
        OK(r == INTERPRET_OK && !strcmp(out, "7\n"));
        free(out); free(w2);
    }
    free(src);

    /* LOW1: num() has no length cap - it accepts exactly the source-literal
     * grammar however long, matching the lexer */
    out = run_capture("print(num(\"123456789012345678901234567890123456789012345\"));", &r);
    OK(r == INTERPRET_OK && !strcmp(out, "1.2345678901234567e+44\n"));
    free(out);
    {
        char digits[101];
        memset(digits, '9', 100);
        digits[100] = '\0';
        size_t cap = 160;
        src = malloc(cap);
        snprintf(src, cap, "print(num(\"%s\"));", digits);
        out = run_capture(src, &r);
        OK(r == INTERPRET_OK && !strcmp(out, "1e+100\n"));
        free(out); free(src);
    }

    /* num() on bools: true is 1, false is 0 (documented in the README) */
    out = run_capture("print(num(true), num(false));", &r);
    OK(r == INTERPRET_OK && !strcmp(out, "1 0\n"));
    free(out);

    /* print() is variadic: arguments print space-separated */
    out = run_capture("print(1, \"x\", true, nil);", &r);
    OK(r == INTERPRET_OK && !strcmp(out, "1 x true nil\n"));
    free(out);

    /* LOW2: an unterminated string or block comment reports the line where
     * the construct opened - a trailing newline no longer shifts it, and
     * multi-line strings anchor at the opening quote */
    out = run_capture("let u = \"unterminated\n", &r);
    OK(r == INTERPRET_COMPILE_ERROR && strstr(last_err, "<test>:1: unterminated string") != NULL);
    free(out);
    out = run_capture("let a = 1;\nlet u = \"abc", &r);
    OK(r == INTERPRET_COMPILE_ERROR && strstr(last_err, "<test>:2: unterminated string") != NULL);
    free(out);
    out = run_capture("let s = \"a\nb\nc", &r);
    OK(r == INTERPRET_COMPILE_ERROR && strstr(last_err, "<test>:1: unterminated string") != NULL);
    free(out);
    out = run_capture("print(1);\n/* never\nclosed\n", &r);
    OK(r == INTERPRET_COMPILE_ERROR && strstr(last_err, "<test>:2: unterminated block comment") != NULL);
    free(out);

    /* LOW3: string concatenation is capped (default 256 MB, documented);
     * inject a small cap to exercise the check without 512 MB of heap */
    size_t saved_cap = vm.string_max;
    vm.string_max = 64;
    out = run_capture("print(\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\" + \"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\");", &r);
    OK(r == INTERPRET_RUNTIME_ERROR);
    OK(strstr(vm.errbuf, "string too long") != NULL);
    vm.had_runtime_error = false;
    free(out);
    out = run_capture("print(\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\" + \"bbbbbbbbbbbbbbbbbbbbbbbb\");", &r);
    OK(r == INTERPRET_OK);   /* exactly at the cap is allowed */
    free(out);
    vm.string_max = saved_cap;
    out = run_capture("print(\"ok\" + \"!\");", &r);
    OK(r == INTERPRET_OK && !strcmp(out, "ok!\n"));
    free(out);
    free(p513);
}


/* ---- critic round 6 ---- */
static void critic_r6(void) {
    InterpretResult r;
    char *out, *src;
    char *p513 = src_parens(513);

    /* F1: once a depth guard fires, that one error is the whole report -
     * trailing lexer errors (unterminated string, stray character, open
     * block comment) are no longer cascaded by the error-recovery scan. */
    const char *tails[] = { "; \"unterminated", "; @", "; /* never closed" };
    const char *forbidden[] = { "unterminated", "unexpected character", "unterminated" };
    for (int i = 0; i < 3; i++) {
        size_t cap = strlen(p513) + strlen(tails[i]) + 32;
        src = malloc(cap);
        snprintf(src, cap, "let x = %s%s", p513, tails[i]);
        out = run_capture(src, &r);
        OK(r == INTERPRET_COMPILE_ERROR);
        OK(strstr(last_err, "expression nested too deeply") != NULL);
        OK(strstr(last_err, forbidden[i]) == NULL);   /* no cascade */
        {   /* exactly one error line (each ends in a newline) */
            int nl = 0;
            for (const char *p = last_err; *p; p++) if (*p == '\n') nl++;
            OK(nl == 1);
        }
        free(out); free(src);
    }
    free(p513);

    /* F3: the documented limits are all real, each with its clean error */
    src = malloc(1 << 20);   /* scratch space for the generated programs below */
    {   /* 255 parameters pass, 256 are "at most 255 parameters" */
        char *w = src;
        w += sprintf(w, "fn f(");
        for (int i = 0; i < 255; i++) w += sprintf(w, i ? ", a%d" : "a%d", i);
        w += sprintf(w, ") { return a0; }");
        out = run_capture(src, &r);
        OK(r == INTERPRET_OK);
        free(out);
        w += sprintf(w, " fn g(");
        for (int i = 0; i < 256; i++) w += sprintf(w, i ? ", b%d" : "b%d", i);
        w += sprintf(w, ") { return b0; }");
        out = run_capture(src, &r);
        OK(r == INTERPRET_COMPILE_ERROR && strstr(last_err, "at most 255 parameters") != NULL);
        free(out);
    }
    {   /* 256 call arguments: "at most 255 arguments" */
        char *w = src;
        w += sprintf(w, "fn h(");
        for (int i = 0; i < 255; i++) w += sprintf(w, i ? ", a%d" : "a%d", i);
        w += sprintf(w, ") { return a0; } h(");
        for (int i = 0; i < 256; i++) w += sprintf(w, i ? ", %d" : "%d", i);
        w += sprintf(w, ");");
        out = run_capture(src, &r);
        OK(r == INTERPRET_COMPILE_ERROR && strstr(last_err, "at most 255 arguments") != NULL);
        free(out);
    }
    {   /* 255 locals per function pass, the 256th is "too many local
         * variables" (the frame holds 256 slots; slot 0 is the function) */
        char *w = src;
        w += sprintf(w, "fn f() { ");
        for (int i = 0; i < 255; i++) w += sprintf(w, "let v%d = %d; ", i, i);
        w += sprintf(w, "} fn g() { ");
        for (int i = 0; i < 256; i++) w += sprintf(w, "let w%d = %d; ", i, i);
        w += sprintf(w, "}");
        out = run_capture(src, &r);
        OK(r == INTERPRET_COMPILE_ERROR && strstr(last_err, "too many local variables") != NULL);
        free(out);
    }
    free(src);
    /* 257 captures in one closure: mid's 3 locals plus its 254 upvalues
     * captured from outer - "too many captured variables", and no other
     * limit trips first */
    {
        size_t cap = 254 * 24 + 254 * 8 + 256;
        src = malloc(cap);
        char *w = src;
        w += sprintf(w, "fn outer() { ");
        for (int i = 0; i < 254; i++) w += sprintf(w, "let o%d = %d; ", i, i);
        w += sprintf(w, "fn mid() { let m0 = 0; let m1 = 0; let m2 = 0; fn inner() { let t = m0 + m1 + m2");
        for (int i = 0; i < 254; i++) w += sprintf(w, " + o%d", i);
        w += sprintf(w, "; return t; } return inner; } return mid; }");
        out = run_capture(src, &r);
        OK(r == INTERPRET_COMPILE_ERROR && strstr(last_err, "too many captured variables") != NULL);
        OK(strstr(last_err, "too many local variables") == NULL);
        free(out); free(src);
    }
    {   /* round-7 F1: list literals have no element-count cap - they build
         * incrementally, two stack slots at any length, so even 100k
         * elements run at the top level and inside a call */
        /* block scope: the list must be collectable afterwards so r4's
         * live-byte accounting does not see it */
        src = src_with_nils("{ let x = [", 65536, "]; print(len(x)); }");
        out = run_capture(src, &r);
        OK(r == INTERPRET_OK && !strcmp(out, "65536\n"));
        free(out); free(src);
        src = src_with_nils("fn f() { return [", 100000, "]; } print(len(f()));");
        out = run_capture(src, &r);
        OK(r == INTERPRET_OK && !strcmp(out, "100000\n"));
        free(out); free(src);
    }
    {   /* 65537 constants in one chunk: "too many constants in one chunk".
         * One flat sum of distinct literals - global declarations would
         * intern 65k names and permanently grow the (never-shrinking)
         * intern table, which r4's live-byte accounting must not see. */
        size_t cap = 65537 * 8 + 32;
        src = malloc(cap);
        char *w = src;
        w += sprintf(w, "let x = 0");
        for (int i = 1; i < 65537; i++) w += sprintf(w, " + %d", i);
        w += sprintf(w, ";");
        out = run_capture(src, &r);
        OK(r == INTERPRET_COMPILE_ERROR && strstr(last_err, "too many constants in one chunk") != NULL);
        free(out); free(src);
    }
    {   /* a loop body over 64 KB of bytecode: "loop body too large";
         * an if body over 64 KB: "jump too far" */
        size_t cap = 7000 * 12 + 64;
        src = malloc(cap);
        char *w = src;
        w += sprintf(w, "let i = 0; while (i < 1) { ");
        for (int i = 0; i < 7000; i++) w += sprintf(w, "i = i + 1; ");
        w += sprintf(w, "}");
        out = run_capture(src, &r);
        OK(r == INTERPRET_COMPILE_ERROR && strstr(last_err, "loop body too large") != NULL);
        free(out);
        w = src;
        w += sprintf(w, "let i = 0; if (1) { ");
        for (int i = 0; i < 7000; i++) w += sprintf(w, "i = i + 1; ");
        w += sprintf(w, "}");
        out = run_capture(src, &r);
        OK(r == INTERPRET_COMPILE_ERROR && strstr(last_err, "jump too far") != NULL);
        free(out); free(src);
    }
    out = run_capture("print(\"alive\");", &r);   /* VM healthy after all of it */
    OK(r == INTERPRET_OK && !strcmp(out, "alive\n"));
    free(out);
    gc_collect();   /* drop the big failed-compile garbage before r4's GC accounting checks */
}

int main(void) {
    vm_init(stdout, stderr);
    lexer_basics();
    critic_r6();
    critic_r5();
    values_basics();
    table_basics();
    chunk_basics();
    critic_r4();
    critic_r3();
    critic_r2();
    critic_r1();
    repl_echo();
    end_to_end();
    /* GC: allocate a pile of garbage, force a collection, keep working */
    for (int i = 0; i < 2000; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "garbage-string-number-%d", i);
        string_copy(buf, (int)strlen(buf));
    }
    gc_collect();
    InterpretResult r;
    char *out = run_capture("print(\"alive\" + \"!\");", &r);
    OK(r == INTERPRET_OK && !strcmp(out, "alive!\n"));
    free(out);
    printf("%ld checks passed\n", checks);
    vm_free();
    return 0;
}

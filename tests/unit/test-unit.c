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

int main(void) {
    vm_init(stdout, stderr);
    lexer_basics();
    values_basics();
    table_basics();
    chunk_basics();
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

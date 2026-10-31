#include "compiler.h"
#include "lexer.h"
#include "vm.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define MAX_LOCALS   256
#define MAX_UPVALUES 256

typedef struct {
    Token name;
    int depth;       /* scope depth, -1 = declared but not yet defined */
    bool captured;
} Local;

typedef struct {
    uint8_t index;
    bool is_local;
} Upval;

typedef enum { FN_SCRIPT, FN_FUNC } FnKind;

typedef struct Compiler {
    struct Compiler *enclosing;
    ObjFunction *fn;
    FnKind kind;
    Local locals[MAX_LOCALS];
    int local_count;
    Upval upvalues[MAX_UPVALUES];
    int scope_depth;
} Compiler;

typedef struct {
    Lexer lx;
    Token cur;
    Token prev;
    bool had_error;
    bool panic;
    Compiler *compiler;
    bool repl_mode;
} Parser;

static Parser P;

/* ---- errors ---- */
static void error_at(Token *t, const char *fmt, ...) {
    if (P.panic) return;
    P.panic = true;
    P.had_error = true;
    va_list ap; va_start(ap, fmt);
    char msg[256];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (t->type == T_EOF)
        fprintf(vm.err, "line %d, at end: %s\n", t->line, msg);
    else if (t->type == T_ERROR)
        fprintf(vm.err, "line %d: %.*s\n", t->line, t->len, t->start);
    else
        fprintf(vm.err, "line %d, at '%.*s': %s\n", t->line, t->len, t->start, msg);
}

static void advance(void) {
    P.prev = P.cur;
    for (;;) {
        P.cur = lexer_next(&P.lx);
        if (P.cur.type != T_ERROR) break;
        error_at(&P.cur, "%.*s", P.cur.len, P.cur.start);
    }
}

static bool check(TokenType t) { return P.cur.type == t; }
static bool match(TokenType t) { if (!check(t)) return false; advance(); return true; }

static void consume(TokenType t, const char *fmt, ...) {
    if (P.cur.type == t) { advance(); return; }
    va_list ap; va_start(ap, fmt);
    char msg[256];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    error_at(&P.cur, "%s", msg);
}

/* ---- emit ---- */
static Chunk *chunk(void) { return &P.compiler->fn->chunk; }
static int emit(uint8_t b) { return chunk_write(chunk(), b, P.prev.line); }
static void emit2(uint8_t a, uint8_t b) { emit(a); emit(b); }
static void emit16(uint8_t op, uint16_t v) {
    emit(op);
    emit((uint8_t)(v >> 8));
    emit((uint8_t)(v & 0xff));
}
static int emit_jump(uint8_t op) {
    emit(op);
    emit(0xff);
    emit(0xff);
    return chunk()->count - 2;
}
static void patch_jump(int at) {
    int j = chunk()->count - at - 2;
    if (j > UINT16_MAX) error_at(&P.prev, "jump too far");
    chunk_patch16(chunk(), at, (uint16_t)j);
}
static void emit_loop(int to) {
    emit(OP_LOOP);
    int back = chunk()->count - to + 2;
    if (back > UINT16_MAX) error_at(&P.prev, "loop body too large");
    emit((uint8_t)(back >> 8));
    emit((uint8_t)(back & 0xff));
}
static uint16_t make_constant(Value v) {
    int i = chunk_add_constant(chunk(), v);
    if (i > UINT16_MAX) { error_at(&P.prev, "too many constants in one chunk"); return 0; }
    return (uint16_t)i;
}
static uint16_t ident_constant(Token *name) {
    return make_constant(OBJ_VAL(string_copy(name->start, name->len)));
}

/* ---- compiler chain ---- */
static void compiler_init(Compiler *c, FnKind kind) {
    c->enclosing = P.compiler;
    c->fn = function_new();
    c->kind = kind;
    c->local_count = 0;
    c->scope_depth = 0;
    P.compiler = c;
    vm.compilers = (struct Compiler *)c;   /* GC root chain */
    /* slot 0: the function itself (script) or the callee (for calls) */
    Local *l = &c->locals[c->local_count++];
    l->depth = 0;
    l->captured = false;
    l->name.start = "";
    l->name.len = 0;
}

static ObjFunction *compiler_end(void) {
    emit(OP_NIL);
    emit(OP_RETURN);
    ObjFunction *fn = P.compiler->fn;
    vm.compilers = P.compiler->enclosing ? (struct Compiler *)P.compiler->enclosing : NULL;
    P.compiler = P.compiler->enclosing;
    if (P.compiler) vm.compilers = (struct Compiler *)P.compiler;
    return fn;
}

/* ---- scope + locals ---- */
static void begin_scope(void) { P.compiler->scope_depth++; }
static void end_scope(void) {
    P.compiler->scope_depth--;
    while (P.compiler->local_count > 0 &&
           P.compiler->locals[P.compiler->local_count - 1].depth > P.compiler->scope_depth) {
        if (P.compiler->locals[P.compiler->local_count - 1].captured) emit(OP_CLOSE_UPVALUE);
        else emit(OP_POP);
        P.compiler->local_count--;
    }
}

static bool ident_eq(Token *a, Token *b) {
    return a->len == b->len && memcmp(a->start, b->start, (size_t)a->len) == 0;
}

static int resolve_local(Compiler *c, Token *name) {
    for (int i = c->local_count - 1; i >= 0; i--) {
        Local *l = &c->locals[i];
        if (ident_eq(&l->name, name)) {
            if (l->depth == -1)
                error_at(name, "cannot read a variable in its own initializer");
            return i;
        }
    }
    return -1;
}

static int add_upvalue(Compiler *c, uint8_t index, bool is_local) {
    int n = c->fn->upvalue_count;
    for (int i = 0; i < n; i++)
        if (c->upvalues[i].index == index && c->upvalues[i].is_local == is_local)
            return i;
    if (n == MAX_UPVALUES) {
        error_at(&P.prev, "too many captured variables in one function");
        return 0;
    }
    c->upvalues[n].index = index;
    c->upvalues[n].is_local = is_local;
    return c->fn->upvalue_count++;
}

static int resolve_upvalue(Compiler *c, Token *name) {
    if (!c->enclosing) return -1;
    int local = resolve_local(c->enclosing, name);
    if (local != -1) {
        c->enclosing->locals[local].captured = true;
        return add_upvalue(c, (uint8_t)local, true);
    }
    int up = resolve_upvalue(c->enclosing, name);
    if (up != -1) return add_upvalue(c, (uint8_t)up, false);
    return -1;
}

static void declare_local(Token *name) {
    Compiler *c = P.compiler;
    if (c->scope_depth == 0) return;   /* globals live in the table */
    for (int i = c->local_count - 1; i >= 0; i--) {
        Local *l = &c->locals[i];
        if (l->depth != -1 && l->depth < c->scope_depth) break;
        if (ident_eq(&l->name, name))
            error_at(name, "a variable with this name already exists in this scope");
    }
    if (c->local_count == MAX_LOCALS) {
        error_at(name, "too many local variables in one function");
        return;
    }
    Local *l = &c->locals[c->local_count++];
    l->name = *name;
    l->depth = -1;
    l->captured = false;
}

static void define_local(void) {
    if (P.compiler->scope_depth == 0) return;
    P.compiler->locals[P.compiler->local_count - 1].depth = P.compiler->scope_depth;
}

/* named variable get/set: local, then upvalue, then global */
static void named_variable(Token name, bool can_assign);
static void expression(void);
static void statement(void);
static void declaration(void);

/* ---- Pratt table ---- */
typedef enum {
    PREC_NONE, PREC_ASSIGN, PREC_OR, PREC_AND, PREC_EQUALITY,
    PREC_COMPARE, PREC_TERM, PREC_FACTOR, PREC_UNARY, PREC_CALL, PREC_PRIMARY
} Precedence;

typedef void (*PrefixFn)(bool can_assign);
typedef void (*InfixFn)(bool can_assign);

typedef struct {
    PrefixFn prefix;
    InfixFn infix;
    Precedence prec;
} Rule;

static Rule rules[T_EOF + 1];
static Rule *rule(TokenType t) { return &rules[t]; }

static void parse_precedence(Precedence prec) {
    advance();
    PrefixFn prefix = rule(P.prev.type)->prefix;
    if (!prefix) {
        error_at(&P.prev, "expected an expression, got %s", token_name(P.prev.type));
        return;
    }
    bool can_assign = prec <= PREC_ASSIGN;
    prefix(can_assign);
    while (prec <= rule(P.cur.type)->prec) {
        advance();
        rule(P.prev.type)->infix(can_assign);
    }
    if (can_assign && match(T_EQ))
        error_at(&P.prev, "invalid assignment target");
}

static void expression(void) { parse_precedence(PREC_ASSIGN); }

/* ---- prefix parsers ---- */
static void number(bool can_assign) {
    (void)can_assign;
    char buf[64];
    int n = P.prev.len < 63 ? P.prev.len : 63;
    memcpy(buf, P.prev.start, (size_t)n);
    buf[n] = '\0';
    emit16(OP_CONSTANT, make_constant(NUM_VAL(strtod(buf, NULL))));
}

static void string_lit(bool can_assign) {
    (void)can_assign;
    /* unquote and resolve escapes */
    const char *s = P.prev.start + 1;
    int n = P.prev.len - 2;
    char *buf = GROW_ARRAY(char, NULL, n + 1);
    int w = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            i++;
            switch (s[i]) {
                case 'n': buf[w++] = '\n'; break;
                case 't': buf[w++] = '\t'; break;
                case 'r': buf[w++] = '\r'; break;
                case '0': buf[w++] = '\0'; break;
                case '"': buf[w++] = '"'; break;
                case '\\': buf[w++] = '\\'; break;
                default:
                    error_at(&P.prev, "unknown escape '\\%c'", s[i]);
                    buf[w++] = s[i];
            }
        } else buf[w++] = s[i];
    }
    buf[w] = '\0';
    ObjString *str = string_take(buf, w);
    emit16(OP_CONSTANT, make_constant(OBJ_VAL(str)));
}

static void literal(bool can_assign) {
    (void)can_assign;
    switch (P.prev.type) {
        case T_TRUE: emit(OP_TRUE); break;
        case T_FALSE: emit(OP_FALSE); break;
        case T_NIL: emit(OP_NIL); break;
        default: break;
    }
}

static void grouping(bool can_assign) {
    (void)can_assign;
    expression();
    consume(T_RPAREN, "expected ')' after expression");
}

static void unary(bool can_assign) {
    (void)can_assign;
    TokenType op = P.prev.type;
    parse_precedence(PREC_UNARY);
    switch (op) {
        case T_MINUS: emit(OP_NEGATE); break;
        case T_BANG: case T_NOT: emit(OP_NOT); break;
        default: break;
    }
}

static void named_variable(Token name, bool can_assign) {
    int arg = resolve_local(P.compiler, &name);
    uint8_t get_op, set_op;
    if (arg != -1) { get_op = OP_GET_LOCAL; set_op = OP_SET_LOCAL; }
    else if ((arg = resolve_upvalue(P.compiler, &name)) != -1) {
        get_op = OP_GET_UPVALUE; set_op = OP_SET_UPVALUE;
    } else {
        arg = (int)ident_constant(&name);
        if (can_assign && match(T_EQ)) {
            expression();
            emit16(OP_SET_GLOBAL, (uint16_t)arg);
            return;
        }
        emit16(OP_GET_GLOBAL, (uint16_t)arg);
        return;
    }
    if (can_assign && match(T_EQ)) {
        expression();
        emit2(set_op, (uint8_t)arg);
    } else {
        emit2(get_op, (uint8_t)arg);
    }
}

static void variable(bool can_assign) { named_variable(P.prev, can_assign); }

/* ---- infix parsers ---- */
static void binary(bool can_assign) {
    (void)can_assign;
    TokenType op = P.prev.type;
    Rule *r = rule(op);
    parse_precedence((Precedence)(r->prec + 1));
    switch (op) {
        case T_PLUS: emit(OP_ADD); break;
        case T_MINUS: emit(OP_SUB); break;
        case T_STAR: emit(OP_MUL); break;
        case T_SLASH: emit(OP_DIV); break;
        case T_PERCENT: emit(OP_MOD); break;
        case T_EQEQ: emit(OP_EQUAL); break;
        case T_BANGEQ: emit(OP_EQUAL); emit(OP_NOT); break;
        case T_LT: emit(OP_LESS); break;
        case T_LTE: emit(OP_GREATER); emit(OP_NOT); break;   /* a<=b is !(a>b) */
        case T_GT: emit(OP_GREATER); break;
        case T_GTE: emit(OP_LESS); emit(OP_NOT); break;      /* a>=b is !(a<b) */
        default: break;
    }
}

static void and_op(bool can_assign) {
    (void)can_assign;
    int end = emit_jump(OP_JUMP_IF_FALSE);   /* falsy lhs stays as the value */
    emit(OP_POP);                            /* truthy lhs: discard, keep rhs */
    parse_precedence(PREC_AND + 1);
    patch_jump(end);
}

static void or_op(bool can_assign) {
    (void)can_assign;
    /* if lhs is false, discard it and evaluate rhs; else keep lhs */
    int else_j = emit_jump(OP_JUMP_IF_FALSE);
    int end = emit_jump(OP_JUMP);
    patch_jump(else_j);
    emit(OP_POP);
    parse_precedence(PREC_OR + 1);
    patch_jump(end);
}

static void call(bool can_assign) {
    (void)can_assign;
    int argc = 0;
    if (!check(T_RPAREN)) {
        do {
            expression();
            if (argc == 255) error_at(&P.prev, "at most 255 arguments");
            argc++;
        } while (match(T_COMMA));
    }
    consume(T_RPAREN, "expected ')' after arguments");
    emit2(OP_CALL, (uint8_t)argc);
}

static void list_lit(bool can_assign) {
    (void)can_assign;
    int n = 0;
    if (!check(T_RBRACKET)) {
        do {
            expression();
            if (n == UINT16_MAX) { error_at(&P.prev, "list literal too long"); break; }
            n++;
        } while (match(T_COMMA));
    }
    consume(T_RBRACKET, "expected ']' after list elements");
    emit16(OP_BUILD_LIST, (uint16_t)n);
}

static void index_op(bool can_assign) {
    expression();
    consume(T_RBRACKET, "expected ']' after index");
    if (can_assign && match(T_EQ)) {
        expression();
        emit(OP_INDEX_SET);
    } else {
        emit(OP_INDEX_GET);
    }
}

/* ---- functions ---- */
static void function_body(FnKind kind, Token name_tok) {
    Compiler c;
    compiler_init(&c, kind);
    P.compiler->fn->name = kind == FN_SCRIPT ? NULL
        : string_copy(name_tok.start, name_tok.len);
    begin_scope();
    consume(T_LPAREN, "expected '(' after the function name");
    if (!check(T_RPAREN)) {
        do {
            P.compiler->fn->arity++;
            if (P.compiler->fn->arity > 255)
                error_at(&P.prev, "at most 255 parameters");
            consume(T_IDENT, "expected a parameter name");
            declare_local(&P.prev);
            define_local();
        } while (match(T_COMMA));
    }
    consume(T_RPAREN, "expected ')' after parameters");
    consume(T_LBRACE, "expected '{' before the function body");
    while (!check(T_RBRACE) && !check(T_EOF)) declaration();
    consume(T_RBRACE, "expected '}' after the function body");
    ObjFunction *fn = compiler_end();
    emit16(OP_CLOSURE, make_constant(OBJ_VAL(fn)));
    /* c (the child compiler) still holds this function's upvalue wiring -
       P.compiler has already switched back to the enclosing one */
    for (int i = 0; i < fn->upvalue_count; i++) {
        emit(c.upvalues[i].is_local ? 1 : 0);
        emit(c.upvalues[i].index);
    }
}

static void fn_expr(bool can_assign) {
    (void)can_assign;
    /* optional name: used for error messages and stack traces. Recursion
       needs a declaration or a 'let' binding - an fn expression does not
       bind its own name (documented in the README). */
    Token name; name.start = "fn"; name.len = 2; name.line = P.prev.line;
    if (check(T_IDENT)) { advance(); name = P.prev; }
    function_body(FN_FUNC, name);
}

/* ---- statements ---- */
static void expr_statement(void) {
    expression();
    consume(T_SEMICOLON, "expected ';' after expression");
    emit(P.repl_mode ? OP_PRINT_POP : OP_POP);
}

static void var_declaration(void) {
    consume(T_IDENT, "expected a variable name after 'let'");
    Token name = P.prev;
    declare_local(&name);
    uint16_t global = 0;
    if (P.compiler->scope_depth == 0) global = ident_constant(&name);
    if (match(T_EQ)) expression();
    else emit(OP_NIL);
    consume(T_SEMICOLON, "expected ';' after the variable declaration");
    if (P.compiler->scope_depth > 0) define_local();
    else emit16(OP_DEF_GLOBAL, global);
}

static void if_statement(void) {
    consume(T_LPAREN, "expected '(' after 'if'");
    expression();
    consume(T_RPAREN, "expected ')' after the condition");
    int else_j = emit_jump(OP_JUMP_IF_FALSE);
    emit(OP_POP);                    /* condition was true: discard it */
    statement();
    int end_j = emit_jump(OP_JUMP);
    patch_jump(else_j);
    emit(OP_POP);                    /* condition was false: discard it */
    if (match(T_ELSE)) statement();
    patch_jump(end_j);
}

static void while_statement(void) {
    int loop_start = chunk()->count;
    consume(T_LPAREN, "expected '(' after 'while'");
    expression();
    consume(T_RPAREN, "expected ')' after the condition");
    int exit_j = emit_jump(OP_JUMP_IF_FALSE);
    emit(OP_POP);
    statement();
    emit_loop(loop_start);
    patch_jump(exit_j);
    emit(OP_POP);
}

static void for_statement(void) {
    begin_scope();
    consume(T_LPAREN, "expected '(' after 'for'");
    if (match(T_SEMICOLON)) { /* no initializer */ }
    else if (match(T_LET)) {
        /* var declaration without the leading keyword consumed normally */
        consume(T_IDENT, "expected a variable name after 'let'");
        Token name = P.prev;
        declare_local(&name);
        if (match(T_EQ)) expression(); else emit(OP_NIL);
        consume(T_SEMICOLON, "expected ';' after the loop variable");
        define_local();
    } else {
        expression();
        consume(T_SEMICOLON, "expected ';' after the initializer");
        emit(OP_POP);
    }
    int loop_start = chunk()->count;
    int exit_j = -1;
    if (!match(T_SEMICOLON)) {
        expression();
        consume(T_SEMICOLON, "expected ';' after the condition");
        exit_j = emit_jump(OP_JUMP_IF_FALSE);
        emit(OP_POP);
    }
    if (!check(T_RPAREN)) {
        int body_j = emit_jump(OP_JUMP);
        int inc_start = chunk()->count;
        expression();
        consume(T_RPAREN, "expected ')' after the increment");
        emit(OP_POP);
        emit_loop(loop_start);
        loop_start = inc_start;
        patch_jump(body_j);
    } else {
        consume(T_RPAREN, "expected ')' after the for clauses");
    }
    statement();
    emit_loop(loop_start);
    if (exit_j != -1) { patch_jump(exit_j); emit(OP_POP); }
    end_scope();
}

static void return_statement(void) {
    if (P.compiler->kind == FN_SCRIPT) {
        error_at(&P.prev, "'return' outside a function");
        return;
    }
    if (match(T_SEMICOLON)) {
        emit(OP_NIL);
    } else {
        expression();
        consume(T_SEMICOLON, "expected ';' after the return value");
    }
    emit(OP_RETURN);
}

static void block(void) {
    while (!check(T_RBRACE) && !check(T_EOF)) declaration();
    consume(T_RBRACE, "expected '}' after the block");
}

static void fn_declaration(void) {
    consume(T_IDENT, "expected a function name after 'fn'");
    Token name = P.prev;
    declare_local(&name);
    if (P.compiler->scope_depth > 0) define_local();
    uint16_t global = 0;
    if (P.compiler->scope_depth == 0) global = ident_constant(&name);
    function_body(FN_FUNC, name);
    if (P.compiler->scope_depth == 0) emit16(OP_DEF_GLOBAL, global);
}

static void statement(void) {
    if (match(T_IF)) if_statement();
    else if (match(T_WHILE)) while_statement();
    else if (match(T_FOR)) for_statement();
    else if (match(T_RETURN)) return_statement();
    else if (match(T_LBRACE)) { begin_scope(); block(); end_scope(); }
    else expr_statement();
}

static void synchronize(void) {
    P.panic = false;
    while (P.cur.type != T_EOF) {
        if (P.prev.type == T_SEMICOLON) return;
        switch (P.cur.type) {
            case T_LET: case T_IF: case T_WHILE: case T_FOR:
            case T_FN: case T_RETURN: return;
            default: break;
        }
        advance();
    }
}

static void declaration(void) {
    if (match(T_LET)) var_declaration();
    else if (match(T_FN)) fn_declaration();
    else statement();
    if (P.panic) synchronize();
}

/* ---- rule table ---- */
static void rules_init(void) {
    for (int i = 0; i <= T_EOF; i++) { rules[i].prefix = NULL; rules[i].infix = NULL; rules[i].prec = PREC_NONE; }
    rules[T_LPAREN]   = (Rule){ grouping, call, PREC_CALL };
    rules[T_LBRACKET] = (Rule){ list_lit, index_op, PREC_CALL };
    rules[T_MINUS]    = (Rule){ unary, binary, PREC_TERM };
    rules[T_PLUS]     = (Rule){ NULL, binary, PREC_TERM };
    rules[T_STAR]     = (Rule){ NULL, binary, PREC_FACTOR };
    rules[T_SLASH]    = (Rule){ NULL, binary, PREC_FACTOR };
    rules[T_PERCENT]  = (Rule){ NULL, binary, PREC_FACTOR };
    rules[T_BANG]     = (Rule){ unary, NULL, PREC_NONE };
    rules[T_NOT]      = (Rule){ unary, NULL, PREC_NONE };
    rules[T_EQEQ]     = (Rule){ NULL, binary, PREC_EQUALITY };
    rules[T_BANGEQ]   = (Rule){ NULL, binary, PREC_EQUALITY };
    rules[T_LT]       = (Rule){ NULL, binary, PREC_COMPARE };
    rules[T_LTE]      = (Rule){ NULL, binary, PREC_COMPARE };
    rules[T_GT]       = (Rule){ NULL, binary, PREC_COMPARE };
    rules[T_GTE]      = (Rule){ NULL, binary, PREC_COMPARE };
    rules[T_AND]      = (Rule){ NULL, and_op, PREC_AND };
    rules[T_OR]       = (Rule){ NULL, or_op, PREC_OR };
    rules[T_IDENT]    = (Rule){ variable, NULL, PREC_NONE };
    rules[T_STRING]   = (Rule){ string_lit, NULL, PREC_NONE };
    rules[T_NUMBER]   = (Rule){ number, NULL, PREC_NONE };
    rules[T_TRUE]     = (Rule){ literal, NULL, PREC_NONE };
    rules[T_FALSE]    = (Rule){ literal, NULL, PREC_NONE };
    rules[T_NIL]      = (Rule){ literal, NULL, PREC_NONE };
    rules[T_FN]       = (Rule){ fn_expr, NULL, PREC_NONE };
}

ObjFunction *compile(const char *src, bool repl_mode) {
    static bool rules_ready = false;
    if (!rules_ready) { rules_init(); rules_ready = true; }
    lexer_init(&P.lx, src);
    P.had_error = false;
    P.panic = false;
    P.compiler = NULL;
    P.repl_mode = repl_mode;
    Compiler c;
    compiler_init(&c, FN_SCRIPT);
    advance();
    while (!match(T_EOF)) declaration();
    ObjFunction *fn = compiler_end();
    return P.had_error ? NULL : fn;
}

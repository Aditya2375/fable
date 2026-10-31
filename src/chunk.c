#include "chunk.h"
#include "obj.h"
#include "vm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void chunk_init(Chunk *c) {
    c->code = NULL; c->lines = NULL; c->count = 0; c->cap = 0;
    varray_init(&c->constants);
    c->dedupe = NULL; c->dedupe_cap = 0;
}

int chunk_write(Chunk *c, uint8_t byte, int line) {
    if (c->count == c->cap) {
        int ncap = c->cap < 8 ? 8 : c->cap * 2;
        c->code = GROW_ARRAY(uint8_t, c->code, ncap);
        c->lines = GROW_ARRAY(int, c->lines, ncap);
        c->cap = ncap;
    }
    c->code[c->count] = byte;
    c->lines[c->count] = line;
    return c->count++;
}

void chunk_free(Chunk *c) {
    FREE_ARRAY(c->code);
    FREE_ARRAY(c->lines);
    varray_free(&c->constants);
    FREE_ARRAY(c->dedupe);
    chunk_init(c);
}

/* Small chunks dedupe constants with a linear scan; past DEDUPE_MIN an
 * open-addressing hash table (values hashed by type and payload, probed
 * linearly, load kept under 2/3) keeps literal-heavy programs O(n) to
 * compile instead of O(n^2). Lookup equality is exactly values_equal:
 * -0 and 0 are the same constant, a NaN never matches (each NaN literal
 * gets its own slot and stays out of the table), strings and functions
 * dedupe by identity (strings are interned, so identity is content). */
#define DEDUPE_MIN 64

static uint32_t const_hash(Value v) {
    switch (v.type) {
        case VAL_NIL:  return 0x9e3779b9u;
        case VAL_BOOL: return 0x9e3779b9u ^ (AS_BOOL(v) ? 0x85ebca6bu : 0x27d4eb2fu);
        case VAL_NUM: {
            double d = AS_NUM(v);
            if (d == 0) d = 0;              /* -0 and 0 compare equal: one bucket */
            uint64_t b;
            memcpy(&b, &d, sizeof b);
            b ^= b >> 32;
            return (uint32_t)b * 0x9e3779b1u;
        }
        case VAL_OBJ:
            if (AS_OBJ(v)->type == OBJ_STRING) return AS_STRING(v)->hash;
            uint64_t p = (uint64_t)(uintptr_t)AS_OBJ(v);   /* functions: identity */
            p ^= p >> 33;
            return (uint32_t)(p ^ (p >> 32));
    }
    return 0;
}

static bool dedupe_skipped(Value v) {
    return IS_NUM(v) && isnan(AS_NUM(v));   /* NaN never equals anything */
}

static void dedupe_insert(Chunk *c, int idx) {
    int m = c->dedupe_cap - 1;
    int i = (int)(const_hash(c->constants.items[idx]) & (uint32_t)m);
    while (c->dedupe[i]) i = (i + 1) & m;
    c->dedupe[i] = idx + 1;
}

static void dedupe_grow(Chunk *c, int ncap) {
    FREE_ARRAY(c->dedupe);
    c->dedupe = GROW_ARRAY(int, NULL, ncap);
    memset(c->dedupe, 0, (size_t)ncap * sizeof(int));
    c->dedupe_cap = ncap;
    for (int i = 0; i < c->constants.count; i++)
        if (!dedupe_skipped(c->constants.items[i])) dedupe_insert(c, i);
}

int chunk_add_constant(Chunk *c, Value v) {
    /* Shield v across the whole prologue: the table build/grow allocates,
     * and a collection with v only in a C local freed fresh string
     * constants out from under const_hash (round-9 HIGH). Compile-time
     * stack space is guaranteed: compilation only runs before execution. */
    vm_push(v);
    bool tabbed = !dedupe_skipped(v);
    if (c->dedupe_cap == 0) {
        if (c->constants.count < DEDUPE_MIN) {
            for (int i = 0; i < c->constants.count; i++)
                if (values_equal(c->constants.items[i], v)) { vm_pop(); return i; }
            tabbed = false;   /* still under the scan threshold: no table yet */
        } else {
            dedupe_grow(c, 256);
        }
    }
    if (tabbed) {
        int m = c->dedupe_cap - 1;
        int i = (int)(const_hash(v) & (uint32_t)m);
        while (c->dedupe[i]) {
            int at = c->dedupe[i] - 1;
            if (values_equal(c->constants.items[at], v)) { vm_pop(); return at; }
            i = (i + 1) & m;
        }
    }
    varray_push(&c->constants, v);   /* rooted by the shield above */
    vm_pop();
    if (tabbed) {
        /* a grow re-inserts every constant including the new one; only the
         * no-grow path inserts it by itself (round-9 L4) */
        if (c->constants.count > (c->dedupe_cap * 2) / 3)
            dedupe_grow(c, c->dedupe_cap * 2);
        else
            dedupe_insert(c, c->constants.count - 1);
    }
    return c->constants.count - 1;
}

void chunk_patch16(Chunk *c, int at, uint16_t v) {
    c->code[at] = (uint8_t)(v >> 8);
    c->code[at + 1] = (uint8_t)(v & 0xff);
}

/* ---- disassembler (debug builds and tests) ---- */
static int simple(const char *name, int off, FILE *out) {
    fprintf(out, "%s\n", name);
    return off + 1;
}
static int cnst(Chunk *c, const char *name, int off, FILE *out) {
    uint16_t i = (uint16_t)((c->code[off + 1] << 8) | c->code[off + 2]);
    fprintf(out, "%-16s %4d '", name, i);
    value_fprint_repr(out, c->constants.items[i]);
    fprintf(out, "'\n");
    return off + 3;
}
static int byteop(Chunk *c, const char *name, int off, FILE *out) {
    fprintf(out, "%-16s %4d\n", name, c->code[off + 1]);
    return off + 2;
}
static int jump(Chunk *c, const char *name, int sign, int off, FILE *out) {
    uint16_t j = (uint16_t)((c->code[off + 1] << 8) | c->code[off + 2]);
    fprintf(out, "%-16s %4d -> %d\n", name, off, off + 3 + sign * j);
    return off + 3;
}

static int dis_one(Chunk *c, int off, FILE *out) {
    fprintf(out, "%04d ", off);
    if (off > 0 && c->lines[off] == c->lines[off - 1]) fprintf(out, "   | ");
    else fprintf(out, "%4d ", c->lines[off]);
    switch (c->code[off]) {
        case OP_CONSTANT: return cnst(c, "OP_CONSTANT", off, out);
        case OP_NIL: return simple("OP_NIL", off, out);
        case OP_TRUE: return simple("OP_TRUE", off, out);
        case OP_FALSE: return simple("OP_FALSE", off, out);
        case OP_POP: return simple("OP_POP", off, out);
        case OP_PRINT_POP: return simple("OP_PRINT_POP", off, out);
        case OP_DEF_GLOBAL: return cnst(c, "OP_DEF_GLOBAL", off, out);
        case OP_GET_GLOBAL: return cnst(c, "OP_GET_GLOBAL", off, out);
        case OP_SET_GLOBAL: return cnst(c, "OP_SET_GLOBAL", off, out);
        case OP_GET_LOCAL: return byteop(c, "OP_GET_LOCAL", off, out);
        case OP_SET_LOCAL: return byteop(c, "OP_SET_LOCAL", off, out);
        case OP_GET_UPVALUE: return byteop(c, "OP_GET_UPVALUE", off, out);
        case OP_SET_UPVALUE: return byteop(c, "OP_SET_UPVALUE", off, out);
        case OP_EQUAL: return simple("OP_EQUAL", off, out);
        case OP_GREATER: return simple("OP_GREATER", off, out);
        case OP_LTE: return simple("OP_LTE", off, out);
        case OP_GTE: return simple("OP_GTE", off, out);
        case OP_LESS: return simple("OP_LESS", off, out);
        case OP_ADD: return simple("OP_ADD", off, out);
        case OP_SUB: return simple("OP_SUB", off, out);
        case OP_MUL: return simple("OP_MUL", off, out);
        case OP_DIV: return simple("OP_DIV", off, out);
        case OP_MOD: return simple("OP_MOD", off, out);
        case OP_NOT: return simple("OP_NOT", off, out);
        case OP_NEGATE: return simple("OP_NEGATE", off, out);
        case OP_JUMP: return jump(c, "OP_JUMP", 1, off, out);
        case OP_JUMP_IF_FALSE: return jump(c, "OP_JUMP_IF_FALSE", 1, off, out);
        case OP_LOOP: return jump(c, "OP_LOOP", -1, off, out);
        case OP_CALL: return byteop(c, "OP_CALL", off, out);
        case OP_CLOSURE: {
            uint16_t i = (uint16_t)((c->code[off + 1] << 8) | c->code[off + 2]);
            off += 3;
            fprintf(out, "%-16s %4d '", "OP_CLOSURE", i);
            value_fprint_repr(out, c->constants.items[i]);
            fprintf(out, "'\n");
            ObjFunction *fn = AS_FUNCTION(c->constants.items[i]);
            for (int u = 0; u < fn->upvalue_count; u++) {
                int local = c->code[off++];
                int idx = c->code[off++];
                fprintf(out, "%04d    |                     %s %d\n",
                        off - 2, local ? "local" : "upvalue", idx);
            }
            return off;
        }
        case OP_CLOSE_UPVALUE: return simple("OP_CLOSE_UPVALUE", off, out);
        case OP_RETURN: return simple("OP_RETURN", off, out);
        case OP_BUILD_LIST: return simple("OP_BUILD_LIST", off, out);
        case OP_LIST_APPEND: return simple("OP_LIST_APPEND", off, out);
        case OP_INDEX_GET: return simple("OP_INDEX_GET", off, out);
        case OP_INDEX_SET: return simple("OP_INDEX_SET", off, out);
    }
    fprintf(out, "OP_?(%d)\n", c->code[off]);
    return off + 1;
}

void chunk_disassemble(Chunk *c, const char *name, FILE *out) {
    fprintf(out, "== %s ==\n", name);
    for (int off = 0; off < c->count;)
        off = dis_one(c, off, out);
    /* nested function chunks too: -disasm used to show only the top level
     * (round-9 L2) */
    for (int i = 0; i < c->constants.count; i++) {
        Value v = c->constants.items[i];
        if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_FUNCTION) {
            ObjFunction *f = (ObjFunction *)AS_OBJ(v);
            chunk_disassemble(&f->chunk, f->name ? f->name->chars : "<fn>", out);
        }
    }
}

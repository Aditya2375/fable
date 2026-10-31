#include "chunk.h"
#include "obj.h"
#include "vm.h"

#include <stdlib.h>
#include <string.h>

void chunk_init(Chunk *c) {
    c->code = NULL; c->lines = NULL; c->count = 0; c->cap = 0;
    varray_init(&c->constants);
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
    chunk_init(c);
}

int chunk_add_constant(Chunk *c, Value v) {
    /* constant dedupe keeps chunks small and OP_EQUAL fast for literals */
    for (int i = 0; i < c->constants.count; i++)
        if (values_equal(c->constants.items[i], v)) return i;
    vm_push(v);   /* GC: the chunk's array may move it */
    varray_push(&c->constants, v);
    vm_pop();
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
        case OP_BUILD_LIST: {
            uint16_t n = (uint16_t)((c->code[off + 1] << 8) | c->code[off + 2]);
            fprintf(out, "%-16s %4d\n", "OP_BUILD_LIST", n);
            return off + 3;
        }
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
}

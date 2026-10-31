/* Bytecode chunk: instruction bytes with parallel line info and constants. */
#ifndef FABLE_CHUNK_H
#define FABLE_CHUNK_H

#include "value.h"

typedef enum {
    OP_CONSTANT,     /* u16 constant index */
    OP_NIL,
    OP_TRUE,
    OP_FALSE,
    OP_POP,
    OP_PRINT_POP,    /* REPL: pop and echo the value (nil stays silent) */
    OP_DEF_GLOBAL,   /* u16 name constant */
    OP_GET_GLOBAL,
    OP_SET_GLOBAL,
    OP_GET_LOCAL,    /* u8 slot */
    OP_SET_LOCAL,
    OP_GET_UPVALUE,  /* u8 index */
    OP_SET_UPVALUE,
    OP_EQUAL,
    OP_GREATER,
    OP_LESS,
    OP_LTE,            /* real <= (NOT-less is wrong for NaN) */
    OP_GTE,            /* real >= (NOT-greater is wrong for NaN) */
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_NOT,
    OP_NEGATE,
    OP_JUMP,         /* u16 offset */
    OP_JUMP_IF_FALSE,/* u16 offset; does NOT pop (and/or need it) */
    OP_LOOP,         /* u16 offset backwards */
    OP_CALL,         /* u8 arg count */
    OP_CLOSURE,      /* u16 function constant + upvalue byte pairs */
    OP_CLOSE_UPVALUE,
    OP_RETURN,
    OP_BUILD_LIST,   /* u16 element count */
    OP_INDEX_GET,    /* list index -> value */
    OP_INDEX_SET,    /* list index value -> */
} OpCode;

typedef struct {
    uint8_t *code;
    int *lines;
    int count;
    int cap;
    ValueArray constants;
} Chunk;

void chunk_init(Chunk *c);
int  chunk_write(Chunk *c, uint8_t byte, int line); /* returns offset */
void chunk_free(Chunk *c);
int  chunk_add_constant(Chunk *c, Value v);         /* dedupes numbers/strings */
void chunk_patch16(Chunk *c, int at, uint16_t v);
void chunk_disassemble(Chunk *c, const char *name, FILE *out);

#endif

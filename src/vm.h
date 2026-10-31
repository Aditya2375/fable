/* The Fable VM: stack, call frames, globals, interned strings, GC. */
#ifndef FABLE_VM_H
#define FABLE_VM_H

#include "chunk.h"
#include "obj.h"
#include "table.h"

#define STACK_MAX   1024
#define GC_THRESHOLD (1024 * 1024)
#define FRAMES_MAX  64

typedef struct {
    ObjClosure *closure;
    uint8_t *ip;
    Value *slots;          /* frame base in the value stack */
} CallFrame;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR
} InterpretResult;

typedef struct VM {
    Value stack[STACK_MAX];
    Value *top;
    CallFrame frames[FRAMES_MAX];
    int frame_count;
    Table globals;
    Table strings;         /* interned */
    ObjUpvalue *open_upvalues;
    Obj *objects;
    /* GC bookkeeping */
    int gray_count;
    int gray_cap;
    Obj **gray_stack;
    size_t bytes_allocated;
    size_t next_gc;
    int gc_paused; /* >0 disables collection (tests with unrooted handles) */
    struct Compiler *compilers;  /* active compiler chain (GC roots) */
    /* output + error channels (the wasm build points these at the page) */
    FILE *out;
    FILE *err;
    bool repl;             /* expression statements echo their value */
    char errbuf[512];      /* last runtime/compile error */
    bool had_runtime_error;
} VM;

extern VM vm;

void vm_init(FILE *out, FILE *err);
void vm_free(void);
InterpretResult interpret(const char *src, const char *chunk_name);

void vm_push(Value v);
Value vm_pop(void);
Value vm_peek(int depth);

void runtime_error(const char *fmt, ...);

/* allocation - everything heap-bound goes through here (GC hook) */
void *fable_alloc(size_t bytes);
void  fable_free_obj(Obj *o);
void  gc_collect(void);
void  gc_mark_value(Value v);
void  gc_mark_obj(Obj *o);

/* growable arrays routed through the GC's allocator */
#define GROW_ARRAY(type, old, ncount) \
    ((type *)fable_realloc(old, sizeof(type) * (size_t)(ncount)))
#define FREE_ARRAY(p) (fable_realloc((p), 0))
void *fable_realloc(void *p, size_t new_size);

#endif

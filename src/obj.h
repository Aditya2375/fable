/* Heap objects: strings (interned), lists, functions, closures, upvalues,
   natives. Every object sits on the VM's allocation list for the GC. */
#ifndef FABLE_OBJ_H
#define FABLE_OBJ_H

#include "value.h"
#include "chunk.h"

typedef enum {
    OBJ_STRING,
    OBJ_LIST,
    OBJ_FUNCTION,
    OBJ_CLOSURE,
    OBJ_UPVALUE,
    OBJ_NATIVE
} ObjType;

struct Obj {
    ObjType type;
    bool marked;
    Obj *next;
};

struct ObjString {
    Obj obj;
    int len;
    uint32_t hash;
    char chars[];      /* NUL-terminated for cheap interop */
};

struct ObjList {
    Obj obj;
    ValueArray items;
};

struct ObjFunction {
    Obj obj;
    int arity;
    int upvalue_count;
    Chunk chunk;
    ObjString *name;   /* may be NULL for the top-level script */
};

typedef Value (*NativeFn)(int argc, Value *argv, bool *err, char *errbuf, int errcap);

struct ObjNative {
    Obj obj;
    int arity;         /* -1: variadic */
    NativeFn fn;
    const char *name;  /* static C string */
};

struct ObjUpvalue {
    Obj obj;
    Value *location;   /* into the stack while open */
    Value closed;
    ObjUpvalue *next;  /* VM's open-upvalue list */
};

struct ObjClosure {
    Obj obj;
    ObjFunction *fn;
    ObjUpvalue **upvalues;
    int upvalue_count;
};

uint32_t hash_bytes(const char *data, int len);

ObjString *string_copy(const char *chars, int len);
ObjString *string_take(char *chars, int len);   /* adopts the buffer */
ObjList   *list_new(void);
ObjFunction *function_new(void);
ObjNative   *native_new(NativeFn fn, int arity, const char *name);
ObjClosure  *closure_new(ObjFunction *fn);
ObjUpvalue  *upvalue_new(Value *slot);
void obj_free(Obj *o);
void obj_print(FILE *out, Obj *o, bool repr);

/* string <-> number helpers used by natives and OP_ADD */
ObjString *string_of_double(double v);
bool       double_of_string(const char *s, int len, double *out);

#endif

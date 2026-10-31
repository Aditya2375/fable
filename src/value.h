/* Fable values and heap objects. */
#ifndef FABLE_VALUE_H
#define FABLE_VALUE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct Obj Obj;
typedef struct ObjString ObjString;
typedef struct ObjList ObjList;
typedef struct ObjFunction ObjFunction;
typedef struct ObjClosure ObjClosure;
typedef struct ObjUpvalue ObjUpvalue;
typedef struct ObjNative ObjNative;

typedef enum {
    VAL_NIL,
    VAL_BOOL,
    VAL_NUM,
    VAL_OBJ
} ValType;

typedef struct {
    ValType type;
    union {
        bool b;
        double num;
        Obj *obj;
    } as;
} Value;

#define NIL_VAL          ((Value){ VAL_NIL,   { .num = 0 } })
#define BOOL_VAL(x)      ((Value){ VAL_BOOL,  { .b = (x) } })
#define NUM_VAL(x)       ((Value){ VAL_NUM,   { .num = (x) } })
#define OBJ_VAL(x)       ((Value){ VAL_OBJ,   { .obj = (Obj *)(x) } })

#define IS_NIL(v)   ((v).type == VAL_NIL)
#define IS_BOOL(v)  ((v).type == VAL_BOOL)
#define IS_NUM(v)   ((v).type == VAL_NUM)
#define IS_OBJ(v)   ((v).type == VAL_OBJ)

#define AS_BOOL(v)  ((v).as.b)
#define AS_NUM(v)   ((v).as.num)
#define AS_OBJ(v)   ((v).as.obj)
#define AS_STRING(v)  ((ObjString *)AS_OBJ(v))
#define AS_CSTRING(v) (((ObjString *)AS_OBJ(v))->chars)
#define AS_LIST(v)    ((ObjList *)AS_OBJ(v))
#define AS_FUNCTION(v) ((ObjFunction *)AS_OBJ(v))
#define AS_CLOSURE(v)  ((ObjClosure *)AS_OBJ(v))
#define AS_NATIVE(v)   ((ObjNative *)AS_OBJ(v))

bool values_equal(Value a, Value b);
bool value_truthy(Value v);
void value_print(FILE *out, Value v);   /* user-facing rendering */
void value_fprint_repr(FILE *out, Value v); /* inside lists: strings quoted */

/* ---- dynamic value array (constants, list elements) ---- */
typedef struct {
    Value *items;
    int count;
    int cap;
} ValueArray;

void varray_init(ValueArray *a);
void varray_push(ValueArray *a, Value v);
void varray_free(ValueArray *a);

#endif

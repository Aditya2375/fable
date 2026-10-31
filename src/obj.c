#include "obj.h"
#include "vm.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

uint32_t hash_bytes(const char *data, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) {
        h ^= (uint8_t)data[i];
        h *= 16777619;
    }
    return h;
}

static Obj *alloc_obj(size_t size, ObjType type) {
    Obj *o = (Obj *)fable_alloc(size);
    o->type = type;
    o->marked = false;
    o->next = vm.objects;
    vm.objects = o;
    return o;
}

static ObjString *alloc_string(char *chars, int len, uint32_t hash) {
    ObjString *s = (ObjString *)alloc_obj(sizeof(ObjString) + (size_t)len + 1, OBJ_STRING);
    s->len = len;
    s->hash = hash;
    memcpy(s->chars, chars, (size_t)len);
    s->chars[len] = '\0';
    /* own the buffer copy; caller keeps theirs unless string_take */
    return s;
}

ObjString *string_copy(const char *chars, int len) {
    uint32_t hash = hash_bytes(chars, len);
    ObjString *hit = table_find_string(&vm.strings, chars, len, hash);
    if (hit) return hit;
    ObjString *s = alloc_string((char *)chars, len, hash);
    vm_push(OBJ_VAL(s));
    table_set(&vm.strings, s, NIL_VAL);
    vm_pop();
    return s;
}

ObjString *string_take(char *chars, int len) {
    uint32_t hash = hash_bytes(chars, len);
    ObjString *hit = table_find_string(&vm.strings, chars, len, hash);
    if (hit) { FREE_ARRAY(chars); return hit; }
    ObjString *s = alloc_string(chars, len, hash);
    FREE_ARRAY(chars);
    vm_push(OBJ_VAL(s));
    table_set(&vm.strings, s, NIL_VAL);
    vm_pop();
    return s;
}

ObjList *list_new(void) {
    ObjList *l = (ObjList *)alloc_obj(sizeof(ObjList), OBJ_LIST);
    varray_init(&l->items);
    return l;
}

ObjFunction *function_new(void) {
    ObjFunction *f = (ObjFunction *)alloc_obj(sizeof(ObjFunction), OBJ_FUNCTION);
    f->arity = 0;
    f->upvalue_count = 0;
    f->name = NULL;
    chunk_init(&f->chunk);
    return f;
}

ObjNative *native_new(NativeFn fn, int arity, const char *name) {
    ObjNative *n = (ObjNative *)alloc_obj(sizeof(ObjNative), OBJ_NATIVE);
    n->fn = fn;
    n->arity = arity;
    n->name = name;
    return n;
}

ObjClosure *closure_new(ObjFunction *fn) {
    ObjUpvalue **ups = GROW_ARRAY(ObjUpvalue *, NULL, fn->upvalue_count);
    for (int i = 0; i < fn->upvalue_count; i++) ups[i] = NULL;
    ObjClosure *c = (ObjClosure *)alloc_obj(sizeof(ObjClosure), OBJ_CLOSURE);
    c->fn = fn;
    c->upvalues = ups;
    c->upvalue_count = fn->upvalue_count;
    return c;
}

ObjUpvalue *upvalue_new(Value *slot) {
    ObjUpvalue *u = (ObjUpvalue *)alloc_obj(sizeof(ObjUpvalue), OBJ_UPVALUE);
    u->location = slot;
    u->closed = NIL_VAL;
    u->next = NULL;
    return u;
}

void obj_free(Obj *o) {
    switch (o->type) {
        case OBJ_STRING: fable_realloc(o, 0); break;   /* flexible array freed with it */
        case OBJ_LIST: {
            ObjList *l = (ObjList *)o;
            varray_free(&l->items);
            fable_realloc(o, 0);
            break;
        }
        case OBJ_FUNCTION: {
            ObjFunction *f = (ObjFunction *)o;
            chunk_free(&f->chunk);
            fable_realloc(o, 0);
            break;
        }
        case OBJ_CLOSURE: {
            ObjClosure *c = (ObjClosure *)o;
            FREE_ARRAY(c->upvalues);
            fable_realloc(o, 0);
            break;
        }
        case OBJ_UPVALUE: fable_realloc(o, 0); break;
        case OBJ_NATIVE: fable_realloc(o, 0); break;
    }
}

ObjString *string_of_double(double v) {
    char buf[40];
    num_to_str(v, buf, sizeof buf);
    return string_copy(buf, (int)strlen(buf));
}

bool double_of_string(const char *s, int len, double *out) {
    /* strict grammar: no whitespace, no hex, no trailing garbage. Accepts
     * [-]digits[.digits][(e|E)[+|-]digits] plus the exact words inf and nan
     * (so num(str(x)) round-trips for every double). */
    if (len == 0 || len > 40) return false;
    if ((len == 3 && memcmp(s, "inf", 3) == 0) || (len == 4 && memcmp(s, "-inf", 4) == 0) ||
        (len == 3 && memcmp(s, "nan", 3) == 0) || (len == 4 && memcmp(s, "-nan", 4) == 0)) {
        *out = s[0] == '-' ? -(s[1] == 'i' ? INFINITY : NAN) : (s[0] == 'i' ? INFINITY : NAN);
        return true;
    }
    int i = 0;
    if (s[i] == '-' || s[i] == '+') i++;
    int digits = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') { i++; digits++; }
    if (i < len && s[i] == '.') {
        i++;
        while (i < len && s[i] >= '0' && s[i] <= '9') { i++; digits++; }
    }
    if (digits == 0) return false;
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < len && (s[i] == '-' || s[i] == '+')) i++;
        int edigits = 0;
        while (i < len && s[i] >= '0' && s[i] <= '9') { i++; edigits++; }
        if (edigits == 0) return false;
    }
    if (i != len) return false;
    char buf[41];
    memcpy(buf, s, (size_t)len);
    buf[len] = '\0';
    *out = strtod(buf, NULL);
    return true;
}

static void print_function(FILE *out, ObjFunction *f) {
    if (!f->name) { fprintf(out, "<script>"); return; }
    fprintf(out, "<fn %.*s>", f->name->len, f->name->chars);
}

/* Ancestor stack for cycle detection while printing lists. Printing never
 * allocates and the VM is single-threaded, so fixed statics are safe. */
#define PRINT_DEPTH_MAX 64
static const ObjList *print_stack[PRINT_DEPTH_MAX];
static int print_depth;

void obj_print(FILE *out, Obj *o, bool repr) {
    switch (o->type) {
        case OBJ_STRING: {
            ObjString *s = (ObjString *)o;
            if (repr) {
                Value v = OBJ_VAL(s);
                value_fprint_repr(out, v);
            } else {
                fwrite(s->chars, 1, (size_t)s->len, out);   /* strings can hold NUL */
            }
            break;
        }
        case OBJ_LIST: {
            ObjList *l = (ObjList *)o;
            for (int i = 0; i < print_depth; i++) {
                if (print_stack[i] == l) { fprintf(out, "[...]"); return; }   /* cycle */
            }
            if (print_depth == PRINT_DEPTH_MAX) { fprintf(out, "..."); return; }   /* too deep */
            print_stack[print_depth++] = l;
            fprintf(out, "[");
            for (int i = 0; i < l->items.count; i++) {
                if (i) fprintf(out, ", ");
                value_fprint_repr(out, l->items.items[i]);
            }
            fprintf(out, "]");
            print_depth--;
            break;
        }
        case OBJ_FUNCTION: print_function(out, (ObjFunction *)o); break;
        case OBJ_CLOSURE: print_function(out, ((ObjClosure *)o)->fn); break;
        case OBJ_UPVALUE: fprintf(out, "<upvalue>"); break;
        case OBJ_NATIVE: fprintf(out, "<native %s>", ((ObjNative *)o)->name); break;
    }
}

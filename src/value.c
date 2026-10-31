#include "value.h"
#include "obj.h"
#include "vm.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

void varray_init(ValueArray *a) { a->items = NULL; a->count = 0; a->cap = 0; }

void varray_push(ValueArray *a, Value v) {
    if (a->count == a->cap) {
        int ncap = a->cap < 8 ? 8 : a->cap * 2;
        a->items = GROW_ARRAY(Value, a->items, ncap);
        a->cap = ncap;
    }
    a->items[a->count++] = v;
}

void varray_free(ValueArray *a) {
    FREE_ARRAY(a->items);
    varray_init(a);
}

bool values_equal(Value a, Value b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case VAL_NIL: return true;
        case VAL_BOOL: return AS_BOOL(a) == AS_BOOL(b);
        case VAL_NUM: {
            /* NaN never equals anything, not even itself (IEEE) */
            if (isnan(AS_NUM(a)) || isnan(AS_NUM(b))) return false;
            return AS_NUM(a) == AS_NUM(b);
        }
        case VAL_OBJ: return AS_OBJ(a) == AS_OBJ(b);   /* strings interned */
    }
    return false;
}

bool value_truthy(Value v) {
    /* nil and false are false; everything else (including 0 and "") is true */
    if (IS_NIL(v)) return false;
    if (IS_BOOL(v)) return AS_BOOL(v);
    return true;
}

/* Shortest round-trip number formatting: the printed form strtod()s back to
 * the exact same double (so num(str(x)) == x). Integral values with
 * magnitude at most 2^53 (the largest integer magnitude where every value
 * is exactly representable as a double) print as plain integers: they are
 * exact, so the full decimal form is both shortest and what a reader
 * expects. -0 keeps its sign; nan/inf are words. Never casts an
 * out-of-range double to long long (that is UB): the magnitude check runs
 * before any cast, and 2^53 fits in a 64-bit long long. */
void num_to_str(double v, char *buf, size_t n) {
    if (isnan(v)) { snprintf(buf, n, "nan"); return; }
    if (isinf(v)) { snprintf(buf, n, v > 0 ? "inf" : "-inf"); return; }
    if (v == 0) { snprintf(buf, n, signbit(v) ? "-0" : "0"); return; }
    if (fabs(v) <= 9007199254740992.0 /* 2^53 */ && v == (double)(long long)v) {
        snprintf(buf, n, "%lld", (long long)v);   /* 3, not 3.0 */
        return;
    }
    for (int p = 1; p <= 17; p++) {
        snprintf(buf, n, "%.*g", p, v);
        if (strtod(buf, NULL) == v) return;
    }
    snprintf(buf, n, "%.17g", v);
}

static void print_num(FILE *out, double v) {
    char buf[40];
    num_to_str(v, buf, sizeof buf);
    fputs(buf, out);
}

void value_print(FILE *out, Value v) {
    switch (v.type) {
        case VAL_NIL: fprintf(out, "nil"); break;
        case VAL_BOOL: fprintf(out, AS_BOOL(v) ? "true" : "false"); break;
        case VAL_NUM: print_num(out, AS_NUM(v)); break;
        case VAL_OBJ: obj_print(out, AS_OBJ(v), false); break;
    }
}

void value_fprint_repr(FILE *out, Value v) {
    if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING) {
        ObjString *s = AS_STRING(v);
        fprintf(out, "\"");
        for (int i = 0; i < s->len; i++) {
            char c = s->chars[i];
            if (c == '\n') fprintf(out, "\\n");
            else if (c == '\t') fprintf(out, "\\t");
            else if (c == '\r') fprintf(out, "\\r");
            else if (c == '"') fprintf(out, "\\\"");
            else if (c == '\\') fprintf(out, "\\\\");
            else fputc(c, out);
        }
        fprintf(out, "\"");
        return;
    }
    value_print(out, v);
}

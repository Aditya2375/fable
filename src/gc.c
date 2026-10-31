/* Mark-sweep garbage collection.
   Roots: the value stack, call frames (their closures), the globals table,
   open upvalues, and the compiler chain (functions being compiled). */
#include "vm.h"
#include "compiler.h"

#include <stdlib.h>
#include <stdio.h>

/* mirror of the compiler's internal chain; only the fields the GC needs.
   Kept in sync by construction: compiler.c stores the real Compiler* in
   vm.compilers, and the GC only walks ->enclosing and ->fn. */
typedef struct GcCompiler {
    struct GcCompiler *enclosing;
    ObjFunction *fn;
    int pad[64 + 256 + 2];   /* generous: we never touch these */
} GcCompiler;

static void gray_push(Obj *o) {
    if (vm.gray_count == vm.gray_cap) {
        int ncap = vm.gray_cap < 32 ? 32 : vm.gray_cap * 2;
        vm.gray_stack = realloc(vm.gray_stack, sizeof(Obj *) * (size_t)ncap);
        if (!vm.gray_stack) { fprintf(stderr, "fable: out of memory in GC\n"); exit(70); }
        vm.gray_cap = ncap;
    }
    vm.gray_stack[vm.gray_count++] = o;
}

void gc_mark_obj(Obj *o) {
    if (!o || o->marked) return;
    o->marked = true;
    gray_push(o);
}

void gc_mark_value(Value v) {
    if (IS_OBJ(v)) gc_mark_obj(AS_OBJ(v));
}

static void blacken(Obj *o) {
    switch (o->type) {
        case OBJ_STRING: case OBJ_NATIVE: break;
        case OBJ_LIST: {
            ObjList *l = (ObjList *)o;
            for (int i = 0; i < l->items.count; i++) gc_mark_value(l->items.items[i]);
            break;
        }
        case OBJ_FUNCTION: {
            ObjFunction *f = (ObjFunction *)o;
            gc_mark_obj((Obj *)f->name);
            for (int i = 0; i < f->chunk.constants.count; i++)
                gc_mark_value(f->chunk.constants.items[i]);
            break;
        }
        case OBJ_CLOSURE: {
            ObjClosure *c = (ObjClosure *)o;
            gc_mark_obj((Obj *)c->fn);
            for (int i = 0; i < c->upvalue_count; i++)
                gc_mark_obj((Obj *)c->upvalues[i]);
            break;
        }
        case OBJ_UPVALUE:
            gc_mark_value(((ObjUpvalue *)o)->closed);
            break;
    }
}

static void mark_roots(void) {
    for (Value *p = vm.stack; p < vm.top; p++) gc_mark_value(*p);
    for (int i = 0; i < vm.frame_count; i++) gc_mark_obj((Obj *)vm.frames[i].closure);
    for (ObjUpvalue *u = vm.open_upvalues; u; u = u->next) gc_mark_obj((Obj *)u);
    table_mark(&vm.globals);
    /* functions under construction by the compiler chain */
    for (GcCompiler *c = (GcCompiler *)vm.compilers; c; c = c->enclosing)
        gc_mark_obj((Obj *)c->fn);
}

static void trace(void) {
    while (vm.gray_count > 0) {
        Obj *o = vm.gray_stack[--vm.gray_count];
        blacken(o);
    }
}

static void sweep(void) {
    Obj **p = &vm.objects;
    while (*p) {
        Obj *o = *p;
        if (!o->marked) {
            *p = o->next;
            obj_free(o);
        } else {
            o->marked = false;   /* reset for the next cycle */
            p = &o->next;
        }
    }
}

void gc_collect(void) {
    mark_roots();
    trace();
    table_remove_white(&vm.strings);   /* interned strings: weak keys */
    sweep();
    vm.next_gc = vm.bytes_allocated * 2;
    if (vm.next_gc < GC_THRESHOLD) vm.next_gc = GC_THRESHOLD;
}

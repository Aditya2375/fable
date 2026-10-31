#include "vm.h"
#include "compiler.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

VM vm;

const char *type_name(Value v) {
    switch (v.type) {
        case VAL_NIL: return "nil";
        case VAL_BOOL: return "bool";
        case VAL_NUM: return "number";
        case VAL_OBJ:
            switch (AS_OBJ(v)->type) {
                case OBJ_STRING: return "string";
                case OBJ_LIST: return "list";
                case OBJ_FUNCTION: case OBJ_CLOSURE: case OBJ_NATIVE: return "function";
                case OBJ_UPVALUE: return "upvalue";
            }
    }
    return "?";
}

static const char *fmt_num(double v) {
    static char buf[32];
    if (v == (double)(long long)v && fabs(v) < 1e15) snprintf(buf, sizeof buf, "%lld", (long long)v);
    else snprintf(buf, sizeof buf, "%.14g", v);
    return buf;
}

static int vm_stack_depth(void) { return (int)(vm.top - vm.stack); }

/* ---- allocator with GC hook ---- */
void *fable_realloc(void *p, size_t new_size) {
    if (new_size == 0) { free(p); return NULL; }
    if (vm.gc_paused == 0 && vm.bytes_allocated > vm.next_gc) gc_collect();
#ifdef FABLE_GC_STRESS
    if (vm.gc_paused == 0) gc_collect();
#endif
    void *r = realloc(p, new_size);
    if (!r) { fprintf(stderr, "fable: out of memory\n"); exit(70); }
    vm.bytes_allocated += new_size;   /* approximates; shrink not tracked */
    return r;
}

void *fable_alloc(size_t bytes) { return fable_realloc(NULL, bytes); }

/* ---- stack ---- */
void vm_push(Value v) {
    if (vm.top >= vm.stack + STACK_MAX) {
        runtime_error("stack overflow (more than %d values)", STACK_MAX);
        return;
    }
    *vm.top++ = v;
}

Value vm_pop(void) {
    if (vm.top > vm.stack) vm.top--;
    return vm.top[0];
}

Value vm_peek(int depth) { return vm.top[-1 - depth]; }

/* ---- errors ---- */
void runtime_error(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(vm.errbuf, sizeof vm.errbuf, fmt, ap);
    va_end(ap);
    vm.had_runtime_error = true;
}

static void print_trace(void) {
    for (int i = vm.frame_count - 1; i >= 0; i--) {
        CallFrame *f = &vm.frames[i];
        ObjFunction *fn = f->closure->fn;
        long ipoff = (long)(f->ip - fn->chunk.code) - 1;
        int line = (fn->chunk.count && ipoff >= 0) ? fn->chunk.lines[ipoff] : 0;
        if (fn->name)
            fprintf(vm.err, "  in %.*s(), line %d\n", fn->name->len, fn->name->chars, line);
        else
            fprintf(vm.err, "  in script, line %d\n", line);
    }
}

/* ---- natives ---- */
static Value n_print(int argc, Value *argv, bool *err, char *eb, int ec) {
    (void)err; (void)eb; (void)ec;
    for (int i = 0; i < argc; i++) {
        if (i) fprintf(vm.out, " ");
        value_print(vm.out, argv[i]);
    }
    fprintf(vm.out, "\n");
    return NIL_VAL;
}
static Value n_len(int argc, Value *argv, bool *err, char *eb, int ec) {
    (void)argc;
    if (IS_OBJ(argv[0]) && AS_OBJ(argv[0])->type == OBJ_STRING)
        return NUM_VAL(AS_STRING(argv[0])->len);
    if (IS_OBJ(argv[0]) && AS_OBJ(argv[0])->type == OBJ_LIST)
        return NUM_VAL(AS_LIST(argv[0])->items.count);
    *err = true;
    snprintf(eb, (size_t)ec, "len() expects a string or list");
    return NIL_VAL;
}
static Value n_push(int argc, Value *argv, bool *err, char *eb, int ec) {
    (void)argc;
    if (!(IS_OBJ(argv[0]) && AS_OBJ(argv[0])->type == OBJ_LIST)) {
        *err = true; snprintf(eb, (size_t)ec, "push() expects a list as its first argument");
        return NIL_VAL;
    }
    ObjList *l = AS_LIST(argv[0]);
    vm_push(argv[1]);          /* GC shield across varray growth */
    varray_push(&l->items, argv[1]);
    vm_pop();
    return argv[0];
}
static Value n_pop(int argc, Value *argv, bool *err, char *eb, int ec) {
    (void)argc;
    if (!(IS_OBJ(argv[0]) && AS_OBJ(argv[0])->type == OBJ_LIST)) {
        *err = true; snprintf(eb, (size_t)ec, "pop() expects a list");
        return NIL_VAL;
    }
    ObjList *l = AS_LIST(argv[0]);
    if (l->items.count == 0) { *err = true; snprintf(eb, (size_t)ec, "pop() on an empty list"); return NIL_VAL; }
    return l->items.items[--l->items.count];
}
static Value n_str(int argc, Value *argv, bool *err, char *eb, int ec) {
    (void)argc; (void)err; (void)eb; (void)ec;
    if (IS_NUM(argv[0])) return OBJ_VAL(string_of_double(AS_NUM(argv[0])));
    if (IS_BOOL(argv[0])) return OBJ_VAL(string_copy(AS_BOOL(argv[0]) ? "true" : "false", AS_BOOL(argv[0]) ? 4 : 5));
    if (IS_NIL(argv[0])) return OBJ_VAL(string_copy("nil", 3));
    if (IS_OBJ(argv[0]) && AS_OBJ(argv[0])->type == OBJ_STRING) return argv[0];
    /* functions and lists get their display form */
    char buf[64];
    if (IS_OBJ(argv[0]) && AS_OBJ(argv[0])->type == OBJ_LIST) {
        ObjList *l = AS_LIST(argv[0]);
        snprintf(buf, sizeof buf, "[list of %d]", l->items.count);
    } else {
        snprintf(buf, sizeof buf, "<fn>");
    }
    return OBJ_VAL(string_copy(buf, (int)strlen(buf)));
}
static Value n_num(int argc, Value *argv, bool *err, char *eb, int ec) {
    (void)argc;
    if (IS_NUM(argv[0])) return argv[0];
    if (IS_BOOL(argv[0])) return NUM_VAL(AS_BOOL(argv[0]) ? 1 : 0);
    if (IS_OBJ(argv[0]) && AS_OBJ(argv[0])->type == OBJ_STRING) {
        ObjString *s = AS_STRING(argv[0]);
        double v;
        if (double_of_string(s->chars, s->len, &v)) return NUM_VAL(v);
        *err = true; snprintf(eb, (size_t)ec, "num(): '%.*s' is not a number", s->len, s->chars);
        return NIL_VAL;
    }
    *err = true; snprintf(eb, (size_t)ec, "num() expects a string, number or bool");
    return NIL_VAL;
}
static Value n_type(int argc, Value *argv, bool *err, char *eb, int ec) {
    (void)argc; (void)err; (void)eb; (void)ec;
    const char *t = "?";
    switch (argv[0].type) {
        case VAL_NIL: t = "nil"; break;
        case VAL_BOOL: t = "bool"; break;
        case VAL_NUM: t = "number"; break;
        case VAL_OBJ:
            switch (AS_OBJ(argv[0])->type) {
                case OBJ_STRING: t = "string"; break;
                case OBJ_LIST: t = "list"; break;
                case OBJ_FUNCTION: case OBJ_CLOSURE: t = "function"; break;
                case OBJ_NATIVE: t = "function"; break;
                case OBJ_UPVALUE: t = "upvalue"; break;
            }
            break;
    }
    return OBJ_VAL(string_copy(t, (int)strlen(t)));
}
static Value n_clock(int argc, Value *argv, bool *err, char *eb, int ec) {
    (void)argc; (void)argv; (void)err; (void)eb; (void)ec;
    return NUM_VAL((double)clock() / CLOCKS_PER_SEC);
}
static Value n_abs(int argc, Value *argv, bool *err, char *eb, int ec) {
    (void)argc; (void)err; (void)eb; (void)ec;
    if (!IS_NUM(argv[0])) { *err = true; snprintf(eb, (size_t)ec, "abs() expects a number"); return NIL_VAL; }
    return NUM_VAL(fabs(AS_NUM(argv[0])));
}
static Value n_floor(int argc, Value *argv, bool *err, char *eb, int ec) {
    (void)argc; (void)err; (void)eb; (void)ec;
    if (!IS_NUM(argv[0])) { *err = true; snprintf(eb, (size_t)ec, "floor() expects a number"); return NIL_VAL; }
    return NUM_VAL(floor(AS_NUM(argv[0])));
}
static Value n_sqrt(int argc, Value *argv, bool *err, char *eb, int ec) {
    (void)argc; (void)err; (void)eb; (void)ec;
    if (!IS_NUM(argv[0])) { *err = true; snprintf(eb, (size_t)ec, "sqrt() expects a number"); return NIL_VAL; }
    return NUM_VAL(sqrt(AS_NUM(argv[0])));
}

static void define_native(const char *name, NativeFn fn, int arity) {
    ObjString *n = string_copy(name, (int)strlen(name));
    vm_push(OBJ_VAL(n));
    vm_push(OBJ_VAL(native_new(fn, arity, n->chars)));
    table_set(&vm.globals, n, vm_peek(0));
    vm_pop();
    vm_pop();
}

/* ---- VM lifecycle ---- */
void vm_init(FILE *out, FILE *err) {
    memset(&vm, 0, sizeof vm);
    vm.top = vm.stack;
    vm.out = out;
    vm.err = err;
    vm.next_gc = GC_THRESHOLD;
    table_init(&vm.globals);
    table_init(&vm.strings);
    define_native("print", n_print, -1);
    define_native("len", n_len, 1);
    define_native("push", n_push, 2);
    define_native("pop", n_pop, 1);
    define_native("str", n_str, 1);
    define_native("num", n_num, 1);
    define_native("type", n_type, 1);
    define_native("clock", n_clock, 0);
    define_native("abs", n_abs, 1);
    define_native("floor", n_floor, 1);
    define_native("sqrt", n_sqrt, 1);
}

void vm_free(void) {
    table_free(&vm.globals);
    table_free(&vm.strings);
    Obj *o = vm.objects;
    while (o) { Obj *next = o->next; obj_free(o); o = next; }
    free(vm.gray_stack);
    memset(&vm, 0, sizeof vm);
}

void fable_free_obj(Obj *o) { obj_free(o); }

/* ---- calls ---- */
static bool call_value(Value callee, int argc) {
    if (!IS_OBJ(callee)) goto not_callable;
    switch (AS_OBJ(callee)->type) {
        case OBJ_CLOSURE: {
            ObjClosure *c = AS_CLOSURE(callee);
            if (c->fn->arity != argc) {
                runtime_error("'%s' expects %d arguments, got %d",
                              c->fn->name ? c->fn->name->chars : "fn", c->fn->arity, argc);
                return false;
            }
            if (vm.frame_count == FRAMES_MAX) {
                runtime_error("stack overflow (more than %d nested calls)", FRAMES_MAX);
                return false;
            }
            CallFrame *f = &vm.frames[vm.frame_count++];
            f->closure = c;
            f->ip = c->fn->chunk.code;
            f->slots = vm.top - argc - 1;
            return true;
        }
        case OBJ_NATIVE: {
            ObjNative *n = AS_NATIVE(callee);
            if (n->arity >= 0 && n->arity != argc) {
                runtime_error("'%s' expects %d arguments, got %d", n->name, n->arity, argc);
                return false;
            }
            bool err = false;
            char eb[256];
            Value r = n->fn(argc, vm.top - argc, &err, eb, (int)sizeof eb);
            if (err) { runtime_error("%s", eb); return false; }
            vm.top -= argc + 1;
            vm_push(r);
            return true;
        }
        default: break;
    }
not_callable:
    runtime_error("can only call functions, got %s", type_name(callee));
    return false;
}

static ObjUpvalue *capture_upvalue(Value *slot) {
    ObjUpvalue *prev = NULL;
    ObjUpvalue *u = vm.open_upvalues;
    while (u && u->location > slot) { prev = u; u = u->next; }
    if (u && u->location == slot) return u;
    ObjUpvalue *created = upvalue_new(slot);
    created->next = u;
    if (prev) prev->next = created;
    else vm.open_upvalues = created;
    /* keep the open list a GC root via vm.open_upvalues (head) */
    return created;
}

static void close_upvalues(Value *last) {
    while (vm.open_upvalues && vm.open_upvalues->location >= last) {
        ObjUpvalue *u = vm.open_upvalues;
        u->closed = *u->location;
        u->location = &u->closed;
        vm.open_upvalues = u->next;
        u->next = NULL;
    }
}

static bool is_iterable_index(ObjList *l, double idx, int *out) {
    if (idx != floor(idx) || isnan(idx)) return false;
    long long i = (long long)idx;
    if (i < 0) i += l->items.count;   /* negative indexes from the end */
    if (i < 0 || i >= l->items.count) return false;
    *out = (int)i;
    return true;
}

/* ---- main dispatch ---- */
static InterpretResult run(void) {
    CallFrame *frame = &vm.frames[vm.frame_count - 1];
#define READ_BYTE() (*frame->ip++)
#define READ_U16() ((uint16_t)((frame->ip += 2), (frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONST() (frame->closure->fn->chunk.constants.items[READ_U16()])
#define BINARY_NUM(op, sym) do { \
        if (!IS_NUM(vm_peek(0)) || !IS_NUM(vm_peek(1))) { \
            runtime_error("operator '%s' needs two numbers, got %s and %s", \
                          sym, type_name(vm_peek(1)), type_name(vm_peek(0))); \
            goto rt_fail; \
        } \
        double b = AS_NUM(vm_pop()); \
        double a = AS_NUM(vm_pop()); \
        vm_push(NUM_VAL(a op b)); \
    } while (0)
#define COMPARE_NUM(op, sym) do { \
        if (!IS_NUM(vm_peek(0)) || !IS_NUM(vm_peek(1))) { \
            runtime_error("comparison '%s' needs two numbers, got %s and %s", \
                          sym, type_name(vm_peek(1)), type_name(vm_peek(0))); \
            goto rt_fail; \
        } \
        double b = AS_NUM(vm_pop()); \
        double a = AS_NUM(vm_pop()); \
        vm_push(BOOL_VAL(a op b)); \
    } while (0)

    for (;;) {
        if (vm.had_runtime_error) goto rt_fail;
        uint8_t op = READ_BYTE();
        switch (op) {
            case OP_CONSTANT: vm_push(READ_CONST()); break;
            case OP_NIL: vm_push(NIL_VAL); break;
            case OP_TRUE: vm_push(BOOL_VAL(true)); break;
            case OP_FALSE: vm_push(BOOL_VAL(false)); break;
            case OP_POP: vm_pop(); break;
            case OP_PRINT_POP: {
                Value v = vm_pop();
                /* REPL echo skips nil: print() and void-ish calls would
                 * otherwise spam a nil after their real output. */
                if (!IS_NIL(v)) {
                    value_print(vm.out, v);
                    fprintf(vm.out, "\n");
                }
                break;
            }
            case OP_DEF_GLOBAL: {
                ObjString *name = AS_STRING(READ_CONST());
                table_set(&vm.globals, name, vm_peek(0));
                vm_pop();
                break;
            }
            case OP_GET_GLOBAL: {
                ObjString *name = AS_STRING(READ_CONST());
                Value v;
                if (!table_get(&vm.globals, name, &v)) {
                    runtime_error("undefined variable '%s'", name->chars);
                    goto rt_fail;
                }
                vm_push(v);
                break;
            }
            case OP_SET_GLOBAL: {
                ObjString *name = AS_STRING(READ_CONST());
                if (table_set(&vm.globals, name, vm_peek(0))) {
                    table_delete(&vm.globals, name);   /* undefined: roll back */
                    runtime_error("cannot assign to undefined variable '%s'", name->chars);
                    goto rt_fail;
                }
                break;
            }
            case OP_GET_LOCAL: vm_push(frame->slots[READ_BYTE()]); break;
            case OP_SET_LOCAL: frame->slots[READ_BYTE()] = vm_peek(0); break;
            case OP_GET_UPVALUE: vm_push(*frame->closure->upvalues[READ_BYTE()]->location); break;
            case OP_SET_UPVALUE: *frame->closure->upvalues[READ_BYTE()]->location = vm_peek(0); break;
            case OP_EQUAL: {
                Value b = vm_pop(), a = vm_pop();
                vm_push(BOOL_VAL(values_equal(a, b)));
                break;
            }
            case OP_GREATER: COMPARE_NUM(>, ">"); break;
            case OP_LESS: COMPARE_NUM(<, "<"); break;
            case OP_ADD: {
                Value b = vm_peek(0), a = vm_peek(1);
                if (IS_NUM(a) && IS_NUM(b)) {
                    vm_pop(); vm_pop();
                    vm_push(NUM_VAL(AS_NUM(a) + AS_NUM(b)));
                } else if (IS_OBJ(a) && AS_OBJ(a)->type == OBJ_STRING &&
                           IS_OBJ(b) && AS_OBJ(b)->type == OBJ_STRING) {
                    ObjString *s1 = AS_STRING(a), *s2 = AS_STRING(b);
                    int n = s1->len + s2->len;
                    char *buf = GROW_ARRAY(char, NULL, n + 1);
                    memcpy(buf, s1->chars, (size_t)s1->len);
                    memcpy(buf + s1->len, s2->chars, (size_t)s2->len + 1);
                    ObjString *cat = string_take(buf, n);
                    vm_pop(); vm_pop();
                    vm_push(OBJ_VAL(cat));
                } else if (IS_OBJ(a) && AS_OBJ(a)->type == OBJ_LIST &&
                           IS_OBJ(b) && AS_OBJ(b)->type == OBJ_LIST) {
                    ObjList *l = list_new();
                    vm_push(OBJ_VAL(l));   /* GC shield while copying */
                    ObjList *la = AS_LIST(a), *lb = AS_LIST(b);
                    for (int i = 0; i < la->items.count; i++) varray_push(&l->items, la->items.items[i]);
                    for (int i = 0; i < lb->items.count; i++) varray_push(&l->items, lb->items.items[i]);
                    vm_pop();
                    vm_pop(); vm_pop();
                    vm_push(OBJ_VAL(l));
                } else {
                    runtime_error("operator '+' needs two numbers, two strings or two lists, got %s and %s",
                                  type_name(a), type_name(b));
                    goto rt_fail;
                }
                break;
            }
            case OP_SUB: BINARY_NUM(-, "-"); break;
            case OP_MUL: BINARY_NUM(*, "*"); break;
            case OP_DIV: BINARY_NUM(/, "/"); break;
            case OP_MOD: {
                if (!IS_NUM(vm_peek(0)) || !IS_NUM(vm_peek(1))) {
                    runtime_error("operator '%%' needs two numbers");
                    goto rt_fail;
                }
                double b = AS_NUM(vm_pop()), a = AS_NUM(vm_pop());
                if (b == 0) { runtime_error("modulo by zero"); goto rt_fail; }
                vm_push(NUM_VAL(fmod(a, b)));
                break;
            }
            case OP_NOT: vm_push(BOOL_VAL(!value_truthy(vm_pop()))); break;
            case OP_NEGATE:
                if (!IS_NUM(vm_peek(0))) {
                    runtime_error("unary '-' needs a number, got %s", type_name(vm_peek(0)));
                    goto rt_fail;
                }
                vm_push(NUM_VAL(-AS_NUM(vm_pop())));
                break;
            case OP_JUMP: { uint16_t off = READ_U16(); frame->ip += off; break; }
            case OP_JUMP_IF_FALSE: {
                uint16_t off = READ_U16();
                /* does NOT pop: the condition is also the value of and/or.
                   Statement contexts emit an explicit OP_POP. */
                if (!value_truthy(vm_peek(0))) frame->ip += off;
                break;
            }
            case OP_LOOP: { uint16_t off = READ_U16(); frame->ip -= off; break; }
            case OP_CALL: {
                int argc = READ_BYTE();
                if (!call_value(vm_peek(argc), argc)) goto rt_fail;
                frame = &vm.frames[vm.frame_count - 1];
                break;
            }
            case OP_CLOSURE: {
                ObjFunction *fn = AS_FUNCTION(READ_CONST());
                ObjClosure *c = closure_new(fn);
                vm_push(OBJ_VAL(c));
                for (int i = 0; i < c->upvalue_count; i++) {
                    uint8_t is_local = READ_BYTE();
                    uint8_t idx = READ_BYTE();
                    if (is_local) c->upvalues[i] = capture_upvalue(frame->slots + idx);
                    else c->upvalues[i] = frame->closure->upvalues[idx];
                }
                break;
            }
            case OP_CLOSE_UPVALUE:
                close_upvalues(vm.top - 1);
                vm_pop();
                break;
            case OP_RETURN: {
                Value r = vm_pop();
                close_upvalues(frame->slots);
                vm.frame_count--;
                if (vm.frame_count == 0) {
                    vm_pop();   /* the script */
                    return INTERPRET_OK;
                }
                vm.top = frame->slots;
                vm_push(r);
                frame = &vm.frames[vm.frame_count - 1];
                break;
            }
            case OP_BUILD_LIST: {
                uint16_t n = READ_U16();
                ObjList *l = list_new();
                /* elements sit above the new list on the stack: shield first */
                vm_push(OBJ_VAL(l));
                for (int i = (int)vm_stack_depth() - n - 1; i < (int)vm_stack_depth() - 1; i++)
                    varray_push(&l->items, vm.stack[i]);
                vm_pop();                    /* the shield */
                vm.top -= n;                 /* the elements */
                vm_push(OBJ_VAL(l));
                break;
            }
            case OP_INDEX_GET: {
                Value idx = vm_pop();
                Value seq = vm_pop();
                if (!IS_OBJ(seq)) goto bad_index;
                if (AS_OBJ(seq)->type == OBJ_LIST) {
                    ObjList *l = AS_LIST(seq);
                    if (!IS_NUM(idx)) goto bad_index_type;
                    int i;
                    if (!is_iterable_index(l, AS_NUM(idx), &i)) {
                        runtime_error("list index %s out of range (len %d)", fmt_num(AS_NUM(idx)), l->items.count);
                        goto rt_fail;
                    }
                    vm_push(l->items.items[i]);
                } else if (AS_OBJ(seq)->type == OBJ_STRING) {
                    ObjString *s = AS_STRING(seq);
                    if (!IS_NUM(idx)) goto bad_index_type;
                    double d = AS_NUM(idx);
                    long long i = (long long)d;
                    if (d != floor(d) || isnan(d)) goto bad_index_type;
                    if (i < 0) i += s->len;
                    if (i < 0 || i >= s->len) {
                        runtime_error("string index %s out of range (len %d)", fmt_num(d), s->len);
                        goto rt_fail;
                    }
                    vm_push(OBJ_VAL(string_copy(s->chars + i, 1)));
                } else goto bad_index;
                break;
            bad_index:
                runtime_error("can only index a list or string, got %s", type_name(seq));
                goto rt_fail;
            bad_index_type:
                runtime_error("index must be a number, got %s", type_name(idx));
                goto rt_fail;
            }
            case OP_INDEX_SET: {
                Value v = vm_pop();
                Value idx = vm_pop();
                Value seq = vm_pop();
                if (!(IS_OBJ(seq) && AS_OBJ(seq)->type == OBJ_LIST)) {
                    runtime_error("can only index-assign a list, got %s", type_name(seq));
                    goto rt_fail;
                }
                if (!IS_NUM(idx)) {
                    runtime_error("index must be a number, got %s", type_name(idx));
                    goto rt_fail;
                }
                ObjList *l = AS_LIST(seq);
                int i;
                if (!is_iterable_index(l, AS_NUM(idx), &i)) {
                    runtime_error("list index %s out of range (len %d)", fmt_num(AS_NUM(idx)), l->items.count);
                    goto rt_fail;
                }
                l->items.items[i] = v;
                vm_push(v);
                break;
            }
            default:
                runtime_error("corrupt bytecode: unknown opcode %d", op);
                goto rt_fail;
        }
    }

rt_fail:
    if (vm.had_runtime_error) {
        fprintf(vm.err, "line %d: %s\n",
                frame->ip > frame->closure->fn->chunk.code
                    ? frame->closure->fn->chunk.lines[(int)(frame->ip - frame->closure->fn->chunk.code) - 1] : 0,
                vm.errbuf);
        print_trace();
    }
    /* unwind: scripts exit nonzero; the REPL keeps going with a clean stack */
    vm.frame_count = 0;
    vm.top = vm.stack;
    return INTERPRET_RUNTIME_ERROR;

#undef READ_BYTE
#undef READ_U16
#undef READ_CONST
#undef BINARY_NUM
#undef COMPARE_NUM
}

InterpretResult interpret(const char *src, const char *chunk_name) {
    (void)chunk_name;
    ObjFunction *fn = compile(src, vm.repl);
    if (!fn) return INTERPRET_COMPILE_ERROR;
    vm_push(OBJ_VAL(fn));
    ObjClosure *c = closure_new(fn);
    vm_pop();
    vm_push(OBJ_VAL(c));
    vm.had_runtime_error = false;
    if (!call_value(OBJ_VAL(c), 0)) {
        fprintf(vm.err, "line 0: %s\n", vm.errbuf);
        vm_pop();
        return INTERPRET_RUNTIME_ERROR;
    }
    return run();
}

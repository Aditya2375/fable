#include "table.h"
#include "vm.h"

#include <stdlib.h>
#include <string.h>

#define TABLE_MAX_LOAD 0.75

void table_init(Table *t) { t->count = 0; t->cap = 0; t->entries = NULL; }

void table_free(Table *t) {
    FREE_ARRAY(t->entries);
    table_init(t);
}

static Entry *find_entry(Entry *entries, int cap, ObjString *key) {
    uint32_t idx = key->hash & (uint32_t)(cap - 1);
    Entry *tombstone = NULL;
    for (;;) {
        Entry *e = &entries[idx];
        if (!e->key) {
            if (IS_NIL(e->value))
                return tombstone ? tombstone : e;   /* empty */
            if (!tombstone) tombstone = e;          /* deleted */
        } else if (e->key == key) {
            return e;
        }
        idx = (idx + 1) & (uint32_t)(cap - 1);
    }
}

static void grow(Table *t, int cap) {
    Entry *entries = GROW_ARRAY(Entry, NULL, cap);
    for (int i = 0; i < cap; i++) {
        entries[i].key = NULL;
        entries[i].value = NIL_VAL;
    }
    t->count = 0;   /* tombstones evaporate on rehash */
    for (int i = 0; i < t->cap; i++) {
        Entry *e = &t->entries[i];
        if (!e->key) continue;
        Entry *dst = find_entry(entries, cap, e->key);
        dst->key = e->key;
        dst->value = e->value;
        t->count++;
    }
    FREE_ARRAY(t->entries);
    t->entries = entries;
    t->cap = cap;
}

bool table_get(Table *t, ObjString *key, Value *out) {
    if (t->count == 0) return false;
    Entry *e = find_entry(t->entries, t->cap, key);
    if (!e->key) return false;
    *out = e->value;
    return true;
}

bool table_set(Table *t, ObjString *key, Value value) {
    if (t->count + 1 > (int)(t->cap * TABLE_MAX_LOAD)) {
        int cap = t->cap < 8 ? 8 : t->cap * 2;
        grow(t, cap);
    }
    Entry *e = find_entry(t->entries, t->cap, key);
    bool fresh = !e->key;
    if (fresh && IS_NIL(e->value)) t->count++;
    e->key = key;
    e->value = value;
    return fresh;
}

bool table_delete(Table *t, ObjString *key) {
    if (t->count == 0) return false;
    Entry *e = find_entry(t->entries, t->cap, key);
    if (!e->key) return false;
    e->key = NULL;
    e->value = BOOL_VAL(true);   /* tombstone */
    return true;
}

ObjString *table_find_string(Table *t, const char *chars, int len, uint32_t hash) {
    if (t->count == 0) return NULL;
    uint32_t idx = hash & (uint32_t)(t->cap - 1);
    for (;;) {
        Entry *e = &t->entries[idx];
        if (!e->key) {
            if (IS_NIL(e->value)) return NULL;
        } else if (e->key->len == len && e->key->hash == hash &&
                   memcmp(e->key->chars, chars, (size_t)len) == 0) {
            return e->key;
        }
        idx = (idx + 1) & (uint32_t)(t->cap - 1);
    }
}

void table_mark(Table *t) {
    for (int i = 0; i < t->cap; i++) {
        Entry *e = &t->entries[i];
        if (e->key) {
            gc_mark_obj((Obj *)e->key);
            gc_mark_value(e->value);
        }
    }
}

void table_remove_white(Table *t) {
    for (int i = 0; i < t->cap; i++) {
        Entry *e = &t->entries[i];
        if (e->key && !e->key->obj.marked) {
            table_delete(t, e->key);
        }
    }
}

/* Open-addressed hash table: interned strings and globals. */
#ifndef FABLE_TABLE_H
#define FABLE_TABLE_H

#include "value.h"
#include "obj.h"

typedef struct {
    ObjString *key;
    Value value;
} Entry;

typedef struct {
    int count;
    int cap;      /* power of two, 0 when empty */
    Entry *entries;
} Table;

void table_init(Table *t);
void table_free(Table *t);
bool table_get(Table *t, ObjString *key, Value *out);
bool table_set(Table *t, ObjString *key, Value value);   /* true if new key */
bool table_delete(Table *t, ObjString *key);
ObjString *table_find_string(Table *t, const char *chars, int len, uint32_t hash);
void table_mark(Table *t);          /* globals: mark keys and values */
void table_remove_white(Table *t);  /* interned strings: drop unmarked keys */

#endif

/* Single-pass Pratt-parser compiler: source -> bytecode. */
#ifndef FABLE_COMPILER_H
#define FABLE_COMPILER_H

#include "obj.h"

/* Returns the compiled top-level function, or NULL on a compile error.
   Errors are reported to vm.err with line numbers.
   repl_mode: expression statements echo their value instead of discarding. */
ObjFunction *compile(const char *src, bool repl_mode);

#endif

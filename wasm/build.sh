#!/bin/sh
# Rebuild the browser demo in docs/ (requires emsdk on PATH).
set -e
cd "$(dirname "$0")/.."
emcc -O2 -Isrc wasm/wrapper.c \
    src/vm.c src/gc.c src/compiler.c src/lexer.c src/chunk.c \
    src/value.c src/obj.c src/table.c \
    -o docs/fable.js \
    -sEXPORTED_FUNCTIONS=_fable_boot,_fable_exec \
    -sEXPORTED_RUNTIME_METHODS=ccall \
    -sMODULARIZE=1 -sEXPORT_NAME=FableModule \
    -sSTACK_SIZE=4194304 -sALLOW_MEMORY_GROWTH=1

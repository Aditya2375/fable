#!/bin/sh
# Rebuild the browser demo in docs/ (requires emsdk on PATH).
set -e
cd "$(dirname "$0")/.."
emcc -O2 -Isrc wasm/wrapper.c \
    src/vm.c src/gc.c src/compiler.c src/lexer.c src/chunk.c \
    src/value.c src/obj.c src/table.c \
    -o docs/fable.js \
    -sEXPORTED_FUNCTIONS=_fable_boot,_fable_exec_len,_fable_set_echo,_fable_discard_pending,_fable_out_ptr,_fable_out_len,_fable_err_ptr,_fable_err_len,_fable_out_drain,_malloc,_free \
    -sEXPORTED_RUNTIME_METHODS=ccall,writeArrayToMemory,HEAPU8 \
    -sMODULARIZE=1 -sEXPORT_NAME=FableModule \
    -sSTACK_SIZE=4194304 -sALLOW_MEMORY_GROWTH=1

# cache-bust both fetches with the wasm content hash, in lockstep
ver=$(sha256sum docs/fable.wasm | cut -c1-12)
sed -i "s/fable\.js?v=[a-z0-9]\{12\}/fable.js?v=$ver/" docs/index.html
sed -i "s/locateFile: (path) => path + '?v=[a-z0-9]\{12\}'/locateFile: (path) => path + '?v=$ver'/" docs/index.html

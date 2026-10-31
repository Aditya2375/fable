CC      = cc
CFLAGS  = -std=gnu11 -O2 -g -Wall -Wextra -Werror -Isrc
EXTRA   =
SRCS    := src/main.c src/vm.c src/gc.c src/compiler.c src/lexer.c src/chunk.c src/value.c src/obj.c src/table.c
CORE    := src/vm.c src/gc.c src/compiler.c src/lexer.c src/chunk.c src/value.c src/obj.c src/table.c

all: build/fable build/test-unit

build:
	mkdir -p build

build/fable: $(SRCS) | build
	$(CC) $(CFLAGS) $(EXTRA) $(SRCS) -o $@ -lm

build/test-unit: tests/unit/test-unit.c $(CORE) | build
	$(CC) $(CFLAGS) $(EXTRA) tests/unit/test-unit.c $(CORE) -o $@ -lm

test: all
	./build/test-unit
	./tests/run-tests.sh
	./tests/check-docs.sh

sanitize: clean
	$(MAKE) EXTRA="-fsanitize=address,undefined -fno-omit-frame-pointer" test

stress: clean
	$(MAKE) EXTRA="-DFABLE_GC_STRESS" test

clean:
	rm -rf build
	rm -f docs/fable.js docs/fable.wasm

.PHONY: all test clean sanitize stress

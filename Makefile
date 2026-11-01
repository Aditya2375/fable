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

build/test-streams-fallback: tests/test-streams-fallback.c wasm/wrapper.c $(SRCS) | build
	$(CC) $(CFLAGS) $(EXTRA) -Itests/wasm-stub tests/test-streams-fallback.c $(CORE) -o $@ -lm

test: all build/test-streams-fallback
	./build/test-unit
	./build/test-streams-fallback
	./tests/run-tests.sh
	./tests/check-docs.sh

sanitize: clean
	UBSAN_OPTIONS=halt_on_error=1 $(MAKE) EXTRA="-fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -fno-omit-frame-pointer" test

stress: clean
	$(MAKE) EXTRA="-DFABLE_GC_STRESS" test

clean:
	rm -rf build
	# docs/fable.js and docs/fable.wasm are committed artifacts: rebuild them
	# with sh wasm/build.sh, never delete them here

.PHONY: all test clean sanitize stress

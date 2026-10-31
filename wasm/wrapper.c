/* emscripten wrapper: paste the CLI into this translation unit and expose a
 * tiny session API to JavaScript. The VM persists between fable_exec calls
 * so definitions carry over; fable_boot builds a fresh VM. */
#define main fable_cli_main
#include "../src/main.c"
#undef main
#include <emscripten.h>

EMSCRIPTEN_KEEPALIVE
int fable_boot(void) {
    static int booted = 0;
    if (booted) vm_free();
    booted = 1;
    vm_init(stdout, stderr);
    vm.repl = true;   /* expression statements echo, like the terminal REPL */
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int fable_exec(const char *src) {
    InterpretResult r = interpret(src, "<web>");
    vm.had_runtime_error = false;   /* one bad line must not kill the session */
    fflush(stdout);
    fflush(stderr);
    return r == INTERPRET_OK ? 0 : 1;
}

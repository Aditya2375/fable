/* emscripten wrapper: paste the CLI into this translation unit and expose a
 * tiny session API to JavaScript. The VM persists between fable_exec calls
 * so definitions carry over; fable_boot builds a fresh VM. */
#define main fable_cli_main
#include "../src/main.c"
#undef main
#include <emscripten.h>

static void pending_clear(void);

EMSCRIPTEN_KEEPALIVE
int fable_boot(void) {
    static int booted = 0;
    if (booted) vm_free();
    pending_clear();
    booted = 1;
    vm_init(stdout, stderr);
    vm.repl = true;   /* expression statements echo, like the terminal REPL */
    return 0;
}

/* Incomplete input (an open bracket, string or block comment) accumulates
 * until it completes, like the terminal REPL's "... " continuation. */
static char *pending = NULL;
static size_t pending_len = 0, pending_cap = 0;

static void pending_clear(void) {
    free(pending);
    pending = NULL;
    pending_len = pending_cap = 0;
}

/* Whole programs (the demo's example buttons) run without REPL echo;
 * typed lines keep it. */
EMSCRIPTEN_KEEPALIVE
void fable_set_echo(int on) { vm.repl = on != 0; }

EMSCRIPTEN_KEEPALIVE
int fable_exec(const char *src) {
    size_t n = strlen(src);
    if (pending_len + n + 2 > pending_cap) {
        pending_cap = (pending_len + n + 2) * 2;
        pending = realloc(pending, pending_cap);
        if (!pending) return 1;
    }
    memcpy(pending + pending_len, src, n);
    pending_len += n;
    pending[pending_len++] = '\n';
    pending[pending_len] = '\0';
    if (!fable_source_complete(pending)) {
        printf("...\n");
        fflush(stdout);
        return 2;   /* incomplete: waiting for more lines */
    }
    InterpretResult r = interpret(pending, "<web>");
    vm.had_runtime_error = false;   /* one bad line must not kill the session */
    pending_clear();
    fflush(stdout);
    fflush(stderr);
    return r == INTERPRET_OK ? 0 : 1;
}

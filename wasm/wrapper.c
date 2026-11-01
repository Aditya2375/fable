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

/* Drop any half-typed multi-line input. Returns 1 if something was pending,
 * so the page can say what happened. Example buttons call this first:
 * running a program must not wedge the session on an old incomplete line. */
EMSCRIPTEN_KEEPALIVE
int fable_discard_pending(void) {
    int had = pending != NULL;
    pending_clear();
    return had;
}

EMSCRIPTEN_KEEPALIVE
int fable_exec_len(const char *src, int len) {
    /* The JS side marshals an explicit length: a NUL byte is a loud error,
     * never a silent truncation (round-13 F1 - the same bug class the CLI
     * rejects in files and on stdin). The line count covers pending
     * multi-line input, like the terminal REPL's absolute lines. */
    for (int i = 0; i < len; i++) {
        if (src[i] == '\0') {
            int line = 1;
            for (size_t j = 0; j < pending_len; j++) if (pending[j] == '\n') line++;
            for (int j = 0; j < i; j++) if (src[j] == '\n') line++;
            fprintf(stderr, "fable: <web>:%d: NUL byte in input (source must be text)\n", line);
            fflush(stderr);
            pending_clear();   /* a dropped line corrupts any pending input, like the CLI */
            return 1;   /* nothing runs */
        }
    }
    size_t n = (size_t)len;
    if (pending_len + n + 2 > pending_cap) {
        size_t new_cap = (pending_len + n + 2) * 2;
        char *np = realloc(pending, new_cap);   /* temp: a failed realloc must not leak the old buffer */
        if (!np) {
            fprintf(stderr, "fable: <web>: out of memory receiving input\n");
            fflush(stderr);
            pending_clear();   /* the half-received statement cannot complete */
            return 1;
        }
        pending = np;
        pending_cap = new_cap;
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

/* emscripten wrapper: paste the CLI into this translation unit and expose a
 * tiny session API to JavaScript. The VM persists between fable_exec calls
 * so definitions carry over; fable_boot builds a fresh VM. */
#define main fable_cli_main
#include "../src/main.c"
#undef main
#include <emscripten.h>

static void pending_clear(void);

/* Session line numbering, matching the terminal REPL: web_lines counts every
 * input line received this session (accepted or dropped), and pending_start
 * is the 1-based session line where the current pending statement began, so
 * every error - compile, runtime or NUL - names the absolute session line
 * like the CLI's interpret_at(pending, "<repl>", pending_start). */
static int web_lines = 0;
static int pending_start = 1;

EMSCRIPTEN_KEEPALIVE
int fable_boot(void) {
    static int booted = 0;
    if (booted) vm_free();
    pending_clear();
    web_lines = 0;
    pending_start = 1;
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
     * rejects in files and on stdin). */
    int src_lines = 1;
    for (int j = 0; j < len; j++) if (src[j] == '\n') src_lines++;
    for (int i = 0; i < len; i++) {
        if (src[i] == '\0') {
            /* web_lines already counts every received line, pending or not,
             * so the absolute line is the next session line plus any
             * newlines before the NUL inside this call - the same shape and
             * wording as the CLI's "NUL byte in source" */
            int line = web_lines + 1;
            for (int j = 0; j < i; j++) if (src[j] == '\n') line++;
            fprintf(stderr, "fable: <web>:%d: NUL byte in source (source must be text)\n", line);
            fflush(stderr);
            pending_clear();   /* a dropped line corrupts any pending input, like the CLI */
            web_lines += src_lines;   /* the dropped lines still consumed session lines */
            return 1;   /* nothing runs */
        }
    }
    size_t n = (size_t)len;
    if (pending_len + n + 1 >= 65536) {
        /* the same 64 KB cap on accumulated input as the terminal REPL */
        fprintf(stderr, "fable: <web>: input too long\n");
        fflush(stderr);
        pending_clear();
        web_lines += src_lines;
        return 1;
    }
    if (pending_len + n + 2 > pending_cap) {
        size_t new_cap = (pending_len + n + 2) * 2;
        char *np = realloc(pending, new_cap);   /* temp: a failed realloc must not leak the old buffer */
        if (!np) {
            fprintf(stderr, "fable: <web>: out of memory receiving input\n");
            fflush(stderr);
            pending_clear();   /* the half-received statement cannot complete */
            web_lines += src_lines;   /* the dropped lines still consumed session lines */
            return 1;
        }
        pending = np;
        pending_cap = new_cap;
    }
    if (pending_len == 0) pending_start = web_lines + 1;
    memcpy(pending + pending_len, src, n);
    pending_len += n;
    pending[pending_len++] = '\n';
    pending[pending_len] = '\0';
    web_lines += src_lines;
    if (!fable_source_complete(pending)) {
        printf("...\n");
        fflush(stdout);
        return 2;   /* incomplete: waiting for more lines */
    }
    InterpretResult r = interpret_at(pending, "<web>", pending_start);
    vm.had_runtime_error = false;   /* one bad line must not kill the session */
    pending_clear();
    fflush(stdout);
    fflush(stderr);
    return r == INTERPRET_OK ? 0 : 1;
}

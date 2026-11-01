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

/* One input line, exactly the CLI's getline-loop semantics: NUL check with
 * the absolute session line, per-line 64 KB pending cap, append, then run
 * when the accumulated input completes. Returns 1 on error; the session
 * continues either way. */
static int exec_line(const char *line, size_t n) {
    if (memchr(line, 0, n)) {
        fprintf(stderr, "<web>:%d: NUL byte in source (source must be text)\n", web_lines + 1);
        fflush(stderr);
        pending_clear();   /* a dropped line corrupts any pending input, like the CLI */
        web_lines++;
        return 1;
    }
    if (pending_len + n + 1 >= 65536) {
        fprintf(stderr, "<web>: input too long\n");
        fflush(stderr);
        pending_clear();
        web_lines++;
        return 1;
    }
    if (pending_len + n + 2 > pending_cap) {
        size_t new_cap = (pending_len + n + 2) * 2;
        char *np = realloc(pending, new_cap);   /* temp: a failed realloc must not leak the old buffer */
        if (!np) {
            fprintf(stderr, "<web>: out of memory receiving input\n");
            fflush(stderr);
            pending_clear();   /* the half-received statement cannot complete */
            web_lines++;
            return 1;
        }
        pending = np;
        pending_cap = new_cap;
    }
    if (pending_len == 0) pending_start = web_lines + 1;
    memcpy(pending + pending_len, line, n);
    pending_len += n;
    pending[pending_len++] = '\n';
    pending[pending_len] = '\0';
    web_lines++;
    if (!fable_source_complete(pending)) {
        if (vm.repl) { printf("...\n"); fflush(stdout); }   /* typed-line continuation echo */
        return 0;   /* waiting for more lines */
    }
    InterpretResult r = interpret_at(pending, "<web>", pending_start);
    vm.had_runtime_error = false;   /* one bad line must not kill the session */
    pending_clear();
    return r == INTERPRET_OK ? 0 : 1;
}

EMSCRIPTEN_KEEPALIVE
int fable_exec_len(const char *src, int len) {
    /* The JS side marshals an explicit length, so a NUL byte is a loud
     * error, never a silent truncation (round-13 F1). The call is processed
     * line by line with exactly the CLI's getline-loop semantics: a
     * trailing newline terminates the last line instead of adding a blank
     * one (round-18 F1), the 64 KB pending cap is checked per line
     * (round-18 F2), and a dropped line (NUL, over-cap, OOM) drops any
     * pending input while the lines around it still run. */
    int had_error = 0;
    int start = 0, i = 0;
    while (i < len) {
        if (src[i] != '\n') { i++; continue; }
        if (exec_line(src + start, (size_t)(i - start))) had_error = 1;
        start = ++i;
    }
    if (start < len || len == 0)   /* unterminated tail; an empty call is one empty line */
        if (exec_line(src + start, (size_t)(len - start))) had_error = 1;
    fflush(stdout);
    fflush(stderr);
    return had_error ? 1 : (pending_len ? 2 : 0);
}
